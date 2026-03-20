# 040.11-IXFS — Impossible X FileSystem (Native Filesystem)

> **Goal:** Complete the production-grade IXFS native filesystem for Impossible OS.
> IXFS is the **only filesystem in existence** that unifies: Win32-native semantics
> (Alternate Data Streams, ACLs, case-insensitive by default), modern reliability features
> (Copy-on-Write, journaling, per-block checksums, instant snapshots), and advanced
> capabilities (transparent compression, per-file encryption, block deduplication,
> reflinks). IXFS is what makes this OS **impossible** — no other filesystem combines
> all of these features in a single, coherent design.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for bitmap buffers, refcount tables,
> checksum tables, snapshot inode backups, and any buffer > 4 KB. `kmalloc` is ONLY
> for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Existing Implementation:** IXFS already has a working foundation:
> `ixfs_core.c` (disk I/O, buffer cache, CRC32C), `ixfs_alloc.c` (bitmap, block groups),
> `ixfs_inode.c` (inode R/W, vnodes, dir hash), `ixfs_extent.c` (extent-based mapping),
> `ixfs_journal.c` (WAL transactions), `ixfs_cow.c` (CoW, snapshots, refcounts),
> `ixfs_format.c` (mkfs), `ixfs_ops.c` (VFS callbacks), `ixfs_test.c` (test suite).
> This TODO covers **verification of existing features** and **implementation of new ones**.

> [!NOTE]
> **On-disk layout** (4 KiB blocks): Block 0 = Superblock, Blocks 1..N = Block Bitmap,
> N+1..M = Inode Table, M+1..end = Data blocks. Journal, refcount table, snapshot table,
> and checksum table are allocated in reserved regions defined by the superblock.

---

## 1. Superblock & Volume Management (Existing — Verify)

### 1.1 Superblock Verification *(verify)*

**Prompt:** Verify the existing superblock implementation in `ixfs_format.c` and `ixfs_core.c`. Confirm: magic `0x49584653` at offset 0, version 2, 4 KiB block size, 64-bit `s_total_blocks`/`s_free_blocks`, correct bitmap/inode/data region offsets, volume name, journal region, refcount table, snapshot table, checksum table. Verify CRC32C integrity check of the superblock. Verify the superblock fits in 512 bytes. Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify superblock"`. Add notes directly in this TODO section.

- [x] Superblock struct defined: `struct ixfs_superblock` (512 bytes, packed)
- [x] Magic: `IXFS_MAGIC = 0x49584653`
- [x] Version: `IXFS_VERSION = 2`
- [x] Block size: `IXFS_BLOCK_SIZE = 4096`
- [x] 64-bit block counts: `s_total_blocks`, `s_free_blocks`
- [x] Layout fields: `s_bitmap_start`, `s_inode_start`, `s_data_start`
- [x] Volume name: `s_volume_name[32]`
- [x] Journal fields: `s_journal_start`, `s_journal_blocks`, `s_journal_seq`
- [x] CoW fields: `s_refcount_start`, `s_refcount_blocks`, `s_snapshot_start`, `s_snapshot_count`
- [x] Checksum fields: `s_checksum_start`, `s_checksum_blocks`, `s_checksum`
- [ ] Verify: `s_checksum` is CRC32C over bytes [0..111] — correct range?
- [ ] Commit: `"ixfs: verify superblock"`

### 1.2 Volume Resize (Online) *(agent)*

**Prompt:** Implement online volume resizing — grow an IXFS volume while it is mounted. This is important for dynamic disk management (e.g., expanding a VM's disk). To grow: extend the block bitmap to cover new blocks, update `s_total_blocks` and `s_free_blocks` in the superblock, update block group descriptors. Shrinking is harder (requires relocating data from the tail) — defer to future. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: online volume grow"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS can grow online (but only via Windows Disk Management).
> ext4 can grow online via `resize2fs`. FAT32 and exFAT cannot resize at all.
> IXFS supports grow from Disk Manager GUI — simpler than both Windows and Linux.

- [ ] Implement `ixfs_grow(vol, new_total_blocks)`:
  - [ ] Extend block bitmap: allocate additional bitmap blocks, zero-fill
  - [ ] Update `s_total_blocks` and `s_free_blocks`
  - [ ] Update `s_bitmap_blocks` if more bitmap blocks needed
  - [ ] Recalculate and update block group descriptors (`ixfs_init_groups`)
  - [ ] Flush superblock, bitmap, and checksum table to disk
  - [ ] Journal the metadata changes (atomic via WAL)
- [ ] Wire to Disk Manager: "Extend Volume" in right-click menu
- [ ] Test: grow a 100 MB volume to 200 MB, verify new space is usable
- [ ] Commit: `"ixfs: online volume grow"`

---

## 2. Block Allocation & Bitmap (Existing — Verify)

### 2.1 Block Group Allocator Verification *(verify)*

**Prompt:** Verify the existing block group allocator in `ixfs_alloc.c`. Confirm: bitmap set/clear/test operations, block group initialization (`ixfs_init_groups`), locality-aware allocation (`ixfs_alloc_block_near`), bitmap flush to disk. Verify block groups cover `IXFS_BLOCKS_PER_GROUP = 32768` blocks (128 MiB each) up to `IXFS_MAX_BLOCK_GROUPS = 256` (32 GiB max). Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify block allocator"`. Add notes directly in this TODO section.

- [x] `bitmap_set`, `bitmap_clear`, `bitmap_test` — bit manipulation
- [x] `ixfs_init_groups` — calculate block group boundaries
- [x] `ixfs_alloc_block_near` — try preferred group first, fallback
- [x] `ixfs_alloc_block` — allocate from any group
- [x] `ixfs_free_block` — clear bitmap bit, update group counters
- [x] `ixfs_flush_bitmap` — write bitmap to disk
- [ ] Verify: block group `bg_next_free` hint is updated on alloc/free
- [ ] Commit: `"ixfs: verify block allocator"`

---

## 3. Inode System (Existing — Verify + Extend)

### 3.1 Inode Table Verification *(verify)*

**Prompt:** Verify the existing inode system in `ixfs_inode.c`. Confirm: 128-byte inodes (32 per block), mode/type flags, uid/gid, 64-bit size, ctime/mtime/atime, 4 inline extents, overflow extent block pointer, hard link count. Verify vnode allocation/lookup, directory hash index (`ixfs_fnv1a` hash). Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify inode table"`. Add notes directly in this TODO section.

- [x] `struct ixfs_inode` — 128 bytes, packed
- [x] Mode flags: `IXFS_S_FILE`, `IXFS_S_DIR`, `IXFS_S_TYPEMASK`
- [x] Permission bits: Unix-style (9 bits)
- [x] Extents: 4 inline (`i_extents[4]`), overflow via `i_extent_block`
- [x] Timestamps: `i_ctime`, `i_mtime`, `i_atime` (POSIX seconds)
- [x] `ixfs_read_inode`, `ixfs_write_inode` — disk I/O
- [x] `ixfs_get_vnode` — vnode cache lookup/allocate
- [x] `ixfs_fnv1a` — FNV-1a hash for directory index
- [x] `ixfs_hash_build`, `ixfs_hash_lookup` — O(1) dir lookups
- [ ] Verify: `i_links` hard link count incremented/decremented correctly
- [ ] Commit: `"ixfs: verify inode table"`

### 3.2 Extended Inode Attributes *(agent)*

**Prompt:** Extend the 128-byte inode to support additional metadata needed for Win32 compatibility and advanced features. Add: Win32 file attributes (`FILE_ATTRIBUTE_HIDDEN`, `SYSTEM`, `ARCHIVE`, etc.), nanosecond timestamp extensions, file creation time (`i_crtime` — distinct from `i_ctime` which is inode change time), a security descriptor reference (inode of the ACL), an ADS chain pointer (first ADS inode), a compression type field, and an encryption key ID. Use the existing `i_extent_pad` and reserved bytes, or increase inode size to 256 bytes (64 per block → 16 per block). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: extended inode attributes"`. Add notes directly in this TODO section.

> [!WARNING]
> Increasing inode size from 128 → 256 bytes is a **format change** (`IXFS_VERSION → 3`).
> Existing v2 volumes must be mountable in read-only mode by v3 drivers.

- [ ] Design extended inode fields (new or repurposed):
  - [ ] `i_win32_attrs` (4B) — Win32 attribute flags (hidden, system, archive, etc.)
  - [ ] `i_crtime` (4B) — file creation time (Win32 `ftCreationTime`)
  - [ ] `i_mtime_ns` (4B) — nanosecond extension for `i_mtime`
  - [ ] `i_crtime_ns` (4B) — nanosecond extension for `i_crtime`
  - [ ] `i_security_id` (4B) — reference to security descriptor (0 = default)
  - [ ] `i_ads_first` (4B) — inode number of first ADS (0 = none)
  - [ ] `i_compress_type` (1B) — 0=none, 1=LZ4, 2=Zstd
  - [ ] `i_encrypt_key_id` (4B) — encryption key slot (0 = unencrypted)
  - [ ] `i_flags` (4B) — IXFS-specific flags (sparse, immutable, append-only)
  - [ ] `i_file_id` (8B) — unique persistent file identifier (for `GetFileInformationByHandle`)
- [ ] Upgrade `IXFS_VERSION` to 3 for 256-byte inodes
- [ ] Detect v2 volumes on mount → force read-only mode
- [ ] Update `IXFS_INODES_PER_BLOCK` for 256-byte inodes
- [ ] Commit: `"ixfs: extended inode attributes"`

---

## 4. Extent-Based Allocation (Existing — Verify)

### 4.1 Extent Engine Verification *(verify)*

**Prompt:** Verify the existing extent engine in `ixfs_extent.c`. Confirm: `ixfs_get_block` maps file block index → disk block, `ixfs_add_block_to_extent` extends/creates extents, `ixfs_free_all_extents` deallocates. Verify inline data support (`IXFS_INLINE` flag, ≤48 bytes stored in `i_extents` array). Verify overflow extent blocks for files with >4 extents. Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify extent engine"`. Add notes directly in this TODO section.

- [x] `ixfs_get_block` — binary search inline extents, then overflow
- [x] `ixfs_add_block_to_extent` — extend last extent or allocate new
- [x] `ixfs_free_all_extents` — release all blocks
- [x] Inline data: `IXFS_INLINE` flag for ≤48-byte files
- [ ] Verify: overflow extent block tree works for highly fragmented files
- [ ] Verify: extent merging when adjacent blocks are allocated
- [ ] Commit: `"ixfs: verify extent engine"`

---

## 5. Write-Ahead Log / Journal (Existing — Verify)

### 5.1 Journal Verification *(verify)*

**Prompt:** Verify the existing WAL journal in `ixfs_journal.c`. Confirm: journal header at journal block 0, data entries store full block snapshots, commit entries mark transaction completion, recovery replays uncommitted transactions. Verify transaction API: `ixfs_txn_begin`, `ixfs_txn_write` (up to 8 blocks), `ixfs_txn_commit`. Verify checksum on journal entries. Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify WAL journal"`. Add notes directly in this TODO section.

- [x] `struct ixfs_journal_header` — magic, head, tail, seq
- [x] `struct ixfs_journal_entry` — txn_id, type, target block, checksum, data
- [x] `ixfs_journal_init` — initialize journal on mount
- [x] `ixfs_journal_recover` — replay on dirty mount
- [x] `ixfs_txn_begin`, `ixfs_txn_write`, `ixfs_txn_commit`
- [x] Max 8 blocks per transaction (`IXFS_TXN_MAX_ENTRIES`)
- [ ] Verify: journal wraparound works when head > tail
- [ ] Verify: recovery after simulated crash mid-transaction
- [ ] Commit: `"ixfs: verify WAL journal"`

---

## 6. Copy-on-Write & Snapshots (Existing — Verify + Extend)

### 6.1 CoW & Snapshot Verification *(verify)*

**Prompt:** Verify the existing CoW and snapshot system in `ixfs_cow.c`. Confirm: per-block refcount table, CoW on write (`ixfs_cow_block` allocates new block when refcount > 1), snapshot creation (copies inode table, increments refcounts), snapshot restore, snapshot deletion (decrements refcounts, frees blocks at refcount 0). Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify CoW and snapshots"`. Add notes directly in this TODO section.

- [x] `ixfs_refcount_init/load/flush` — per-block reference counting
- [x] `ixfs_cow_block` — allocate new block if refcount > 1
- [x] `ixfs_snapshot_create` — copy inode table, increment refs
- [x] `ixfs_snapshot_list` — enumerate snapshots
- [x] `ixfs_snapshot_restore` — restore inode table from snapshot
- [x] `ixfs_snapshot_delete` — decrement refs, free unreferenced blocks
- [x] Max 8 simultaneous snapshots (`IXFS_MAX_SNAPSHOTS`)
- [ ] Verify: writing to a file after snapshot correctly CoWs the block
- [ ] Verify: deleting a snapshot frees only blocks with refcount → 0
- [ ] Commit: `"ixfs: verify CoW and snapshots"`

### 6.2 Scheduled Automatic Snapshots *(agent)*

**Prompt:** Implement automatic scheduled snapshots — the filesystem takes snapshots at configurable intervals (hourly, daily). Old snapshots are automatically pruned based on a retention policy: keep the last N hourly, daily, weekly snapshots. This enables Windows "Previous Versions" (Shadow Copy) functionality. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: automatic scheduled snapshots"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows Volume Shadow Copy (VSS) is a separate service, not part of
> NTFS itself — and it's unreliable. ext4/Btrfs snapshots require manual `btrfs subvolume snapshot`.
> IXFS having filesystem-level auto-snapshots with configurable retention is unique.

- [ ] Implement snapshot scheduler:
  - [ ] Configurable interval via Registry: `HKLM\SYSTEM\Storage\IXFS\SnapshotInterval` (seconds)
  - [ ] Default: hourly snapshots
  - [ ] Naming: `auto_YYYYMMDD_HHMMSS`
- [ ] Implement retention policy:
  - [ ] Keep last 24 hourly, 7 daily, 4 weekly snapshots
  - [ ] Auto-delete oldest snapshots that exceed policy
- [ ] Wire to Explorer: "Previous Versions" tab in file Properties
  - [ ] List snapshots containing the file
  - [ ] "Restore" button restores from snapshot
- [ ] Commit: `"ixfs: automatic scheduled snapshots"`

---

## 7. Per-Block Checksums & Self-Healing (Existing — Verify + Extend)

### 7.1 Checksum Verification *(verify)*

**Prompt:** Verify the existing per-block checksum system in `ixfs_core.c`. Confirm: `ixfs_crc32c` computes CRC32C, `ixfs_checksum_update` stores checksum on write, `ixfs_checksum_verify` validates on read, `ixfs_checksum_load/flush` persists the checksum table. Verify `ixfs_scrub` walks all blocks and reports corrupted ones. Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify checksums"`. Add notes directly in this TODO section.

- [x] `ixfs_crc32c` — CRC32C computation
- [x] `ixfs_checksum_update` — store checksum on block write
- [x] `ixfs_checksum_verify` — validate checksum on block read
- [x] `ixfs_checksum_load/flush` — persist checksum table
- [x] `ixfs_scrub` — full-volume integrity scan
- [ ] Verify: checksum mismatch logs error and returns failure
- [ ] Verify: scrub reports count of corrupt vs clean blocks
- [ ] Commit: `"ixfs: verify checksums"`

### 7.2 Self-Healing with Redundant Metadata *(agent)*

**Prompt:** Implement self-healing for critical metadata: superblock, bitmap, and inode table. Store a backup copy of the superblock at the last block of the volume. Store a backup of the first bitmap block at a reserved location. On checksum failure of these critical structures, automatically fall back to the backup copy and log a warning. This is similar to ZFS self-healing but targeted at critical metadata only. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: self-healing metadata"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS has no self-healing (relies on chkdsk). ext4 has journal
> replay but no data self-healing. ZFS has full self-healing but is complex and resource-heavy.
> IXFS provides targeted self-healing for the structures that matter most, with zero overhead.

- [ ] Write backup superblock at volume last block on every superblock flush
- [ ] On superblock checksum failure → read backup → restore if valid → log warning
- [ ] Write backup of bitmap block 0 at a reserved location (after snapshot table)
- [ ] On bitmap corruption → attempt recovery from backup
- [ ] Log: `[IXFS] SELF-HEAL: Primary superblock corrupt, restored from backup`
- [ ] Wire to Disk Manager: show self-healing events in volume health panel
- [ ] Commit: `"ixfs: self-healing metadata"`

---

## 8. VFS Integration (Existing — Verify)

### 8.1 VFS Callbacks Verification *(verify)*

**Prompt:** Verify the existing VFS integration in `ixfs_ops.c`. Confirm all `vfs_ops` callbacks work: `open`, `close`, `read`, `write`, `readdir`, `finddir`, `create`, `unlink`, `rename`, `mkdir`, `rmdir`, `stat`. Verify directory entries (`struct ixfs_dir_entry` — 64 bytes, 64 per block). Verify the mount sequence in `ixfs_init`. Run `bash scripts/build.sh clean`, and commit as `"ixfs: verify VFS callbacks"`. Add notes directly in this TODO section.

- [x] `ixfs_file_ops` — file VFS callbacks
- [x] `ixfs_dir_ops` — directory VFS callbacks
- [x] `ixfs_init` — mount: read superblock, bitmap, journal, refcounts, checksums
- [x] `ixfs_format` — mkfs: create superblock, bitmap, root dir
- [x] Directory entries: 64 bytes each (4B inode + 252B name)
- [ ] Verify: all 12 VFS callbacks return correct error codes
- [ ] Verify: creating + deleting files updates bitmap and inode table atomically
- [ ] Commit: `"ixfs: verify VFS callbacks"`

---

## 9. Alternate Data Streams (Win32 Feature)

### 9.1 Named Data Streams *(agent)*

**Prompt:** Implement Alternate Data Streams (ADS) — the ability to store multiple named data streams on a single file, accessible via the `filename:streamname` syntax. NTFS ADS is used by: web browsers (`:Zone.Identifier`), Outlook (`:OLE...` streams), and the Windows Attachment Manager. IXFS implements ADS by creating hidden "stream inodes" chained from the parent file's `i_ads_first` field. Each stream inode is a regular file-type inode with a special stream-name directory entry in a per-file ADS directory. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: alternate data streams"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Only NTFS supports ADS natively. ext4, FAT32, exFAT do not.
> IXFS having native ADS means all Win32 applications that use streams work correctly
> — including the critical `:Zone.Identifier` for download security.

- [ ] Define ADS chain: `inode->i_ads_first` → first stream inode
  - [ ] Stream inodes have `i_mode = IXFS_S_FILE | IXFS_S_STREAM` (new type flag)
  - [ ] Each stream inode stores: stream name (in a parent directory), data (via extents)
  - [ ] Chain: stream inode's `i_ads_first` → next stream inode (linked list)
- [ ] Parse `filename:streamname` syntax in path resolution:
  - [ ] Split on `:` — left part = base file, right part = stream name
  - [ ] Locate base file's stream directory, search for stream name
- [ ] Implement `CreateFile("file:stream", ...)`:
  - [ ] If stream exists → open it
  - [ ] If stream doesn't exist and writing → create new stream inode, link to chain
- [ ] Implement `DeleteFile("file:stream")` — unlink stream inode, free data
- [ ] Well-known streams:
  - [ ] `Zone.Identifier` — browser download security tag
  - [ ] `$OBJECT_ID` — Windows object identity
- [ ] `FindFirstStreamW` / `FindNextStreamW` — enumerate all streams on a file
- [ ] On file deletion: cascade-delete all attached streams
- [ ] Test: create file with ADS, verify stream data survives file copy on IXFS
- [ ] Commit: `"ixfs: alternate data streams"`

---

## 10. Security Descriptors & ACLs (Win32 Feature)

### 10.1 DACL/SACL Security Descriptors *(agent)*

**Prompt:** Implement Win32-compatible security descriptors on IXFS files. Each file can have an associated security descriptor containing: Owner SID, Group SID, DACL (who can access), and SACL (audit trail). Store them as separate "security inodes" — deduplicated, since many files share the same descriptor. The `i_security_id` field in the inode references a security descriptor object by its hash. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: security descriptors"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS stores security descriptors in `$Secure` with dedup.
> ext4 uses POSIX ACLs (different model). FAT32/exFAT have no security at all.
> IXFS natively supports Win32 DACLs/SACL — installers and enterprise apps "just work".

- [ ] Define security descriptor storage:
  - [ ] Hash-indexed security descriptor table (stored in reserved inodes)
  - [ ] On `SetFileSecurity()`: compute hash of descriptor, reuse if exists, else create
  - [ ] `i_security_id` in inode → index into security table
- [ ] Implement `GetFileSecurity(hFile, lpSecurityDescriptor)`:
  - [ ] Read security descriptor from table by `i_security_id`
  - [ ] Return self-relative `SECURITY_DESCRIPTOR`
- [ ] Implement `SetFileSecurity(hFile, lpSecurityDescriptor)`:
  - [ ] Parse descriptor, compute hash, store in table
  - [ ] Update `i_security_id` in inode
- [ ] Default security: new files inherit parent directory's DACL
- [ ] Well-known SIDs: `S-1-5-18` (SYSTEM), `S-1-5-32-544` (Administrators), etc.
- [ ] Commit: `"ixfs: security descriptors"`

---

## 11. Hard Links & Symbolic Links (Win32 Feature)

### 11.1 Hard Links *(agent)*

**Prompt:** Implement Win32 `CreateHardLink` — multiple directory entries pointing to the same inode. The `i_links` field already tracks hard link count. When a hard link is created, add a new directory entry in the target directory pointing to the same inode and increment `i_links`. When any link is deleted, decrement `i_links`. When `i_links` reaches 0, delete the inode and free its data blocks. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: hard links"`. Add notes directly in this TODO section.

- [ ] Implement `CreateHardLink(lpFileName, lpExistingFileName)`:
  - [ ] Resolve existing file to inode
  - [ ] Create new directory entry in target directory pointing to same inode
  - [ ] Increment `i_links`
  - [ ] Flush inode and target directory (journal transaction)
- [ ] On unlink: decrement `i_links`. If > 0 → file survives
- [ ] On `i_links == 0` → free inode, extents, ADS, security ref
- [ ] `GetFileInformationByHandle.nNumberOfLinks` → return `i_links`
- [ ] Restriction: hard links must be on same volume (same IXFS partition)
- [ ] Commit: `"ixfs: hard links"`

### 11.2 Symbolic Links & Reparse Points *(agent)*

**Prompt:** Implement symbolic links via reparse points. A symlink inode has type `IXFS_S_SYMLINK` (new type) and stores the target path in its data (inline for short paths ≤48 bytes, extent-based for long paths). Support both relative and absolute symlinks. Implement `CreateSymbolicLink` API. Follow symlinks during path resolution (with loop detection — max 8 follows). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: symbolic links"`. Add notes directly in this TODO section.

- [ ] Define `IXFS_S_SYMLINK = 0xA000` — new inode type
- [ ] Symlink data = target path (UTF-16LE or UTF-8)
  - [ ] Short: inline in `i_extents` (≤48 bytes)
  - [ ] Long: stored in data blocks via extents
- [ ] Implement `CreateSymbolicLink(lpSymlinkFileName, lpTargetFileName, dwFlags)`:
  - [ ] `dwFlags & SYMBOLIC_LINK_FLAG_DIRECTORY` — for directory symlinks
  - [ ] Create inode with `IXFS_S_SYMLINK` type, write target path
- [ ] Path resolution: on encountering symlink → read target → resolve recursively
  - [ ] Loop detection: max 8 symlink follows per path resolution
- [ ] `ReadFile` / `GetFileAttributes` on symlink → follows by default
- [ ] Commit: `"ixfs: symbolic links"`

---

## 12. Transparent Compression *(🚀 Impossible OS Feature)*

### 12.1 Per-File LZ4/Zstd Compression *(agent)*

**Prompt:** Implement transparent, per-file compression. When a file is marked for compression (via `DeviceIoControl` with `FSCTL_SET_COMPRESSION`), IXFS compresses data blocks before writing and decompresses on read. Use LZ4 for fast compression (real-time, low CPU) and Zstd for high-ratio compression. Compression unit = 1 block (4 KiB). The inode's `i_compress_type` field selects the algorithm. Compressed blocks smaller than the original are stored in-place with a header indicating compressed size. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: transparent compression"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS uses LZ77 compression (slow, old algorithm from 1993).
> Btrfs uses Zstd (good ratio but high CPU). IXFS offers BOTH LZ4 (fastest) and Zstd
> (best ratio) as selectable options, defaulting to LZ4 for transparent speed.

- [ ] Implement LZ4 compressor/decompressor (or port minimal LZ4 implementation)
- [ ] Implement Zstd decompressor (for optional high-ratio mode)
- [ ] Compression unit: 1 block (4096 bytes)
- [ ] Compressed block format: `{ uint16_t compressed_size, uint8_t algo, uint8_t pad, data[] }`
- [ ] On write with compression enabled:
  - [ ] Compress block → if `compressed_size < block_size * 0.9` → store compressed
  - [ ] If compression doesn't save ≥10% → store uncompressed
- [ ] On read: check block header → decompress if needed → return to caller
- [ ] `i_compress_type`: `0` = none, `1` = LZ4, `2` = Zstd
- [ ] `FSCTL_SET_COMPRESSION` / `FSCTL_GET_COMPRESSION` DeviceIoControl
- [ ] `FILE_ATTRIBUTE_COMPRESSED` set in `i_win32_attrs` when compressed
- [ ] Commit: `"ixfs: transparent compression"`

---

## 13. Per-File Encryption *(🚀 Impossible OS Feature)*

### 13.1 AES-256 Transparent Encryption *(agent)*

**Prompt:** Implement transparent, per-file AES-256-XTS encryption. When a file or directory is marked for encryption, all data blocks are encrypted with a per-file key. The per-file key is stored encrypted by a master key (derived from user password via PBKDF2). The `i_encrypt_key_id` field references a key slot in the volume's key table. Encrypted files are readable only after the volume is unlocked with the correct master key. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: per-file encryption"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS EFS (Encrypting File System) requires certificates and is
> notoriously unreliable on reinstall. ext4 fscrypt requires `fscryptctl` CLI setup.
> IXFS per-file encryption is simpler: set encrypted attribute → enter master password → done.

- [ ] Define key table: stored in reserved blocks after snapshot table
  - [ ] Key slot: `{ key_id, wrapped_key[32], salt[16], flags }`
  - [ ] Master key: derived from user password via PBKDF2 (100K iterations)
  - [ ] Per-file key: random AES-256 key, wrapped by master key
- [ ] On write (encrypted file):
  - [ ] Unwrap per-file key using master key
  - [ ] Encrypt data block with AES-256-XTS (tweak = block number)
  - [ ] Write encrypted block to disk
- [ ] On read (encrypted file):
  - [ ] Read encrypted block → decrypt with per-file key → return to caller
- [ ] On mount: prompt for master password to unlock volume
- [ ] `FILE_ATTRIBUTE_ENCRYPTED` set in `i_win32_attrs`
- [ ] Directories: encrypting a directory encrypts all new files within it
- [ ] Commit: `"ixfs: per-file encryption"`

---

## 14. Block-Level Deduplication *(🚀 Impossible OS Feature)*

### 14.1 Inline Deduplication *(agent)*

**Prompt:** Implement block-level inline deduplication. When writing a block, compute its hash (xxHash64). Check a dedup hash table — if a block with the same hash already exists, reuse the existing block via CoW refcounting instead of allocating a new one. This is especially effective for VM images, build artifacts, and copied files. Uses the existing refcount infrastructure from CoW snapshots. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: inline block deduplication"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS and ext4 have no deduplication at all. ZFS has dedup but it
> requires enormous RAM (each entry uses ~320 bytes). Btrfs has offline dedup only.
> IXFS inline dedup is lightweight because it reuses the CoW refcount table.

- [ ] Build dedup hash table: `{ xxhash64 → block_number }` (in-memory, rebuild on mount)
- [ ] On block write:
  - [ ] Compute xxHash64 of block data
  - [ ] Check hash table for existing block
  - [ ] If found: compare data byte-for-byte (hash collision check)
    - [ ] If match: increment refcount of existing block, use it
    - [ ] If collision: allocate new block normally
  - [ ] If not found: allocate new block, insert into hash table
- [ ] Configurable: `HKLM\SYSTEM\Storage\IXFS\EnableDedup` (default: off for < 1 GB volumes)
- [ ] Telemetry: log dedup ratio: `[IXFS] Dedup: %u blocks deduplicated, saved %llu bytes`
- [ ] Commit: `"ixfs: inline block deduplication"`

---

## 15. Reflinks (Instant File Copy) *(🚀 Impossible OS Feature)*

### 15.1 Reflink Copy *(agent)*

**Prompt:** Implement reflinks — instant, zero-copy file cloning via `CopyFileEx` with `COPY_FILE_COPY_SYMLINK`. When reflink-copying a file, create a new inode pointing to the SAME data blocks, incrementing their refcounts. Writes to either file trigger CoW. This makes `Copy-Paste` of large files instant. Uses existing CoW refcount infrastructure. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: reflink copy"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** NTFS has "block cloning" since Server 2016 (limited). ext4 has no
> reflinks. Btrfs/XFS have reflinks but require specific API. IXFS exposes reflinks
> transparently through `CopyFile` — the user just copies a file and it's instant.

- [ ] Implement `ixfs_reflink(vol, src_inode, dst_dir, dst_name)`:
  - [ ] Create new inode with same metadata (size, type, timestamps)
  - [ ] Copy extent list from source (NOT the data blocks)
  - [ ] Increment refcount for every data block
  - [ ] Create directory entry in destination
- [ ] Hook into `CopyFile` VFS implementation:
  - [ ] If both source and destination are on same IXFS volume → reflink
  - [ ] If cross-volume → traditional byte copy
- [ ] First write to either file after reflink → CoW triggers automatically
- [ ] Test: reflink a 1 GB file → verify instant, verify independent after write
- [ ] Commit: `"ixfs: reflink copy"`

---

## 16. Online Defragmentation *(🚀 Impossible OS Feature)*

### 16.1 Extent Consolidation *(agent)*

**Prompt:** Implement online defragmentation — consolidate fragmented extents while the volume is mounted. Walk files with high extent counts, allocate a new contiguous block range, copy data, update the inode's extent list. Use CoW so the old blocks remain valid until the new extent is committed. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: online defragmentation"`. Add notes directly in this TODO section.

- [ ] Implement `ixfs_defrag_file(vol, ino)`:
  - [ ] Count extents — if ≤ 2 → already well-allocated, skip
  - [ ] Calculate total blocks needed
  - [ ] Allocate contiguous range via `ixfs_alloc_block_near` (same group)
  - [ ] Copy data from old extents to new contiguous range
  - [ ] Update inode with single new extent
  - [ ] Free old blocks (respecting refcounts for CoW)
  - [ ] All within a journal transaction
- [ ] Background defragmenter:
  - [ ] Walk inode table, find files with > 4 extents
  - [ ] Defragment during idle time
  - [ ] Configurable: `HKLM\SYSTEM\Storage\IXFS\AutoDefrag` (default: on)
- [ ] Wire to Disk Manager: "Defragment" button on IXFS volumes
- [ ] Commit: `"ixfs: online defragmentation"`

---

## 17. Format Tool & Disk Manager Integration

### 17.1 IXFS Format Wizard *(agent)*

**Prompt:** Verify and extend `ixfs_format()` to support all new features. The format tool must create: superblock (v3), block bitmap, inode table (256-byte inodes), root directory, journal region, refcount table, snapshot table, checksum table, security descriptor table, key table (encryption), and backup superblock. Wire to Disk Manager as "Format → IXFS" option. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: format tool v3"`. Add notes directly in this TODO section.

- [ ] Extend `ixfs_format()` for v3:
  - [ ] Calculate sizes for all reserved regions
  - [ ] Write v3 superblock with all new fields
  - [ ] Initialize security descriptor table (default DACL)
  - [ ] Initialize encryption key table (empty)
  - [ ] Write backup superblock at last block
  - [ ] Initialize CRC32C checksums for all written blocks
- [ ] Configurable options:
  - [ ] Inode count (auto-calculated or manual)
  - [ ] Journal size (default 64 KiB, configurable)
  - [ ] Volume label
  - [ ] Enable/disable encryption, compression, dedup at format time
- [ ] Wire to Disk Manager: "Format" dialog with IXFS option
- [ ] Commit: `"ixfs: format tool v3"`

---

## 18. Volume Health Dashboard *(🚀 Impossible OS Feature)*

### 18.1 Unified Health Panel *(agent)*

**Prompt:** Aggregate IXFS volume health into a Disk Manager panel. Show: superblock consistency (primary vs backup), journal state (clean/dirty), snapshot count and age, block checksum failures from last scrub, dedup ratio, free space, fragmentation level. Provide one-click "Scrub" button and "Repair" for fixable issues. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ixfs: volume health dashboard"`. Add notes directly in this TODO section.

- [ ] Health metrics:
  - [ ] Superblock: primary vs backup consistency ✅/❌
  - [ ] Journal: clean (all committed) ✅ / dirty (needs replay) ⚠️
  - [ ] Checksums: blocks failing CRC32C from last scrub
  - [ ] Free space: percentage + absolute
  - [ ] Fragmentation: average extents per file
  - [ ] Snapshots: count, oldest, newest
  - [ ] Dedup ratio: deduplicated blocks / total blocks
  - [ ] Encryption: encrypted files count
- [ ] Health score: "Healthy" / "Needs Attention" / "Critical"
- [ ] One-click actions: "Scrub", "Defragment", "Create Snapshot"
- [ ] Commit: `"ixfs: volume health dashboard"`

---

## 19. Testing & Validation

### 19.1 IXFS Comprehensive Test Suite *(agent)*

**Prompt:** Create a comprehensive test suite for all IXFS features. Test via `ixfs_test_performance()` and QEMU boot tests. Cover: basic file CRUD, large files (>4 GiB), deep directory trees, hard links, symlinks, ADS, security descriptors, snapshots (create/restore/delete), CoW behavior, compression, dedup, reflinks, journal recovery, checksum scrubbing, and online defragmentation. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"test: IXFS comprehensive test suite"`. Add notes directly in this TODO section.

- [ ] Basic CRUD: create, read, write, delete, rename files and directories
- [ ] Large file: write > 4 GiB, verify 64-bit size handling
- [ ] Hard links: create link, verify shared inode, delete one, verify other survives
- [ ] Symlinks: create, follow, detect loop (8 max)
- [ ] ADS: create `file:stream`, write/read stream, delete stream
- [ ] Security: set DACL, verify `GetFileSecurity` returns correct descriptor
- [ ] Snapshots: create snapshot, modify file, restore, verify original content
- [ ] CoW: snapshot → write → verify old snapshot has original data
- [ ] Compression: enable on file, write, read back, verify content matches
- [ ] Dedup: write same data to two files, verify block reuse
- [ ] Reflinks: clone file, write to clone, verify original unchanged
- [ ] Journal recovery: kill mid-write, remount, verify no corruption
- [ ] Scrub: corrupt a block, run scrub, verify detection
- [ ] Defrag: fragment file with 10+ extents, defrag, verify ≤ 2 extents
- [ ] Commit: `"test: IXFS comprehensive test suite"`

---

## Priority Order

| ⭐ | Priority | Section | Description |
| -- |----------|---------|-------------|
| 💎 | 🔴 P0 | 1.1 Superblock | Verify — foundation of entire filesystem |
| 💎 | 🔴 P0 | 2.1 Block Allocator | Verify — all writes depend on this |
| 💎 | 🔴 P0 | 3.1 Inode Table | Verify — all file access depends on this |
| 💎 | 🔴 P0 | 4.1 Extent Engine | Verify — file data mapping |
| 💎 | 🔴 P0 | 5.1 Journal | Verify — crash safety |
| 💎 | 🔴 P0 | 6.1 CoW & Snapshots | Verify — data integrity |
| 💎 | 🔴 P0 | 7.1 Checksums | Verify — corruption detection |
| 💎 | 🔴 P0 | 8.1 VFS Callbacks | Verify — usability |
| 💎 | 🟠 P1 | 3.2 Extended Inode | Enable — v3 format with new metadata fields |
| 💎 | 🟠 P1 | 9.1 Alternate Data Streams | Win32 compat — browser downloads, app streams |
| 💎 | 🟠 P1 | 10.1 Security Descriptors | Win32 compat — ACLs for enterprise apps |
| 💎 | 🟠 P1 | 11.1 Hard Links | Win32 compat — WinSxS, VC++ redist |
| 💎 | 🟠 P1 | 11.2 Symbolic Links | Win32 compat — developer tools, junctions |
| 💎 | 🟠 P1 | 17.1 Format Tool v3 | Enable — can't use v3 features without formatter |
| ⭐ | 🟡 P2 | 12.1 Compression | Performance — save space + reduce I/O |
| ⭐ | 🟡 P2 | 14.1 Deduplication | Storage — similar files share blocks |
| ⭐ | 🟡 P2 | 15.1 Reflinks | UX — instant file copy |
| ⭐ | 🟡 P2 | 16.1 Defragmentation | Performance — consolidate extents |
| ⭐ | 🟡 P2 | 1.2 Online Volume Grow | Management — dynamic disk expansion |
| ⭐ | 🟡 P2 | 7.2 Self-Healing | Reliability — auto-repair corruption |
| ⭐ | 🟢 P3 | 6.2 Auto Snapshots | UX — "Previous Versions" without VSS |
| ⭐ | 🟢 P3 | 13.1 Encryption | Security — per-file AES-256 |
| ⭐ | 🟢 P3 | 18.1 Health Dashboard | Monitoring — unified volume health |
| 💎 | 🟢 P3 | 19.1 Test Suite | Quality — automated validation |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## Boot Integration Gotchas (IXFS as Root Partition)

> [!CAUTION]
> **IXFS is a custom filesystem — the kernel MUST have the IXFS driver fully initialized
> before attempting to mount `C:\`.** This is non-negotiable. Unlike NTFS or FAT32 which
> have drivers baked into every OS, IXFS exists only in Impossible OS. If the boot
> sequence tries to mount `C:\` before the driver is ready, the result is `"C:\ not mounted"`.

### Known Failure Modes

| # | Failure Mode                        | Symptom                                      | Root Cause                                                                             | Fix                                                                              |
| - | ----------------------------------- | -------------------------------------------- | -------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| 1 | **No boot-time IXFS driver**        | `C:\` seen as RAW partition                   | IXFS parsing logic not compiled into kernel or not loaded before mount                 | Ensure `ixfs_init()` runs in `boot_storage.c` BEFORE `partition_scan_and_mount()` |
| 2 | **Bootloader blindness**            | Kernel never starts                          | Bootloader can't read IXFS to find `kernel.exe`                                        | Keep bootloader + kernel on FAT32 EFI partition; IXFS is the data partition only  |
| 3 | **Hyper-V Gen 2 SCSI controller**   | Block device not visible                     | IXFS driver initialized but VMBus/StorVSC not ready → no block device to read from     | Initialize VMBus → StorVSC → re-scan partitions (fixed in commit `cd6f749`)       |
| 4 | **Missing partition type GUID**     | Partition scanner skips IXFS partition         | IXFS partition has no recognizable MBR type code or GPT GUID                            | Register IXFS GPT GUID in `partition.c`; use magic `0x49584653` as secondary check |
| 5 | **Driver init order race**          | `C:\` mount fails intermittently             | Storage controller init is async; IXFS mount runs before disk is ready                  | Add `blkdev_wait_ready()` or poll for device availability before mount attempt    |

### Boot Sequence Checklist

```
UEFI GOP + Memory Map → ExitBootServices()
    ↓
Kernel entry (identity-mapped, long mode)
    ↓
PMM → VMM → Heap → Interrupts → Timer
    ↓
Storage controller init:
  ├─ QEMU:   VirtIO-SCSI (virtio_init)
  ├─ Bare:   AHCI (ahci_init)
  └─ HyperV: VMBus → StorVSC (vmbus_init → storvsc_init)  ← ⚠️ MUST complete
    ↓
Partition scan: MBR/GPT → detect IXFS magic 0x49584653
    ↓
ixfs_init() → mount C:\    ← ⚠️ FAILS if any step above incomplete
    ↓
Boot splash → Desktop
```

> [!TIP]
> **Troubleshooting tip:** If `C:\` fails to mount, boot with verbose logging enabled
> (`boot.conf: verbose=1`) and check the serial log for the last successful init step.
> Common culprits: StorVSC GPADL creation failure (Hyper-V), AHCI port not spinning up
> (bare metal), or `partition.c` not recognizing the IXFS partition type.

> [!WARNING]
> **Hyper-V Gen 2 is the hardest target.** Generation 2 VMs use Synthetic SCSI (StorVSC)
> exclusively — there is no emulated IDE fallback. The full VMBus → StorVSC → GPADL
> → VSCSI handshake must complete before ANY disk I/O. If VMBus version negotiation
> fails or the ring buffer GPADL isn't created, the disk is simply invisible.
> See commit `cd6f749` and [StorVSC spec](file:///specs/hyper-v/storvsc-synthetic-scsi.md).

---

## Key Files

| File | Purpose |
|------|---------|
| `include/kernel/fs/ixfs.h` | Public API, on-disk structures, constants |
| `src/kernel/fs/ixfs/ixfs_internal.h` | Internal types, function declarations |
| `src/kernel/fs/ixfs/ixfs_core.c` | Disk I/O, buffer cache, CRC32C, string helpers |
| `src/kernel/fs/ixfs/ixfs_alloc.c` | Bitmap, block groups, alloc/free |
| `src/kernel/fs/ixfs/ixfs_inode.c` | Inode R/W, vnodes, directory hash index |
| `src/kernel/fs/ixfs/ixfs_extent.c` | Extent-based block mapping |
| `src/kernel/fs/ixfs/ixfs_journal.c` | WAL journal, transactions, recovery |
| `src/kernel/fs/ixfs/ixfs_cow.c` | Copy-on-Write, refcounts, snapshots |
| `src/kernel/fs/ixfs/ixfs_format.c` | mkfs — format tool |
| `src/kernel/fs/ixfs/ixfs_ops.c` | VFS callbacks (file + directory) |
| `src/kernel/fs/ixfs/ixfs_test.c` | Test suite |
| `src/kernel/fs/ixfs/ixfs_ads.c` | [NEW] Alternate Data Streams |
| `src/kernel/fs/ixfs/ixfs_security.c` | [NEW] Security descriptors / ACLs |
| `src/kernel/fs/ixfs/ixfs_compress.c` | [NEW] LZ4/Zstd transparent compression |
| `src/kernel/fs/ixfs/ixfs_encrypt.c` | [NEW] AES-256-XTS per-file encryption |
| `src/kernel/fs/ixfs/ixfs_dedup.c` | [NEW] Inline block deduplication |
| `src/kernel/fs/ixfs/ixfs_defrag.c` | [NEW] Online defragmentation |
| `src/kernel/fs/partition.c` | IXFS detection (magic `0x49584653`) |

---

## OS Comparison

| ⭐ | Feature                            | 🪟 NTFS                          | 🐧 ext4                     | 🌊 Btrfs/ZFS                 | 🚀 IXFS                              |
| -- | ---------------------------------- | --------------------------------- | ---------------------------- | ----------------------------- | ------------------------------------- |
| 💎 | Max volume size                    | 16 EB (NTFS)                      | 1 EB                         | 256 ZB (ZFS)                  | 64 TiB (64-bit blocks × 4 KiB)       |
| 💎 | Block size                         | 512–64K clusters                  | 1K–64K                       | 4K–128K                       | 4 KiB (page-aligned)                  |
| 💎 | Extent-based allocation            | ✅ Non-resident $DATA             | ✅ Extent tree               | ✅ B-tree extents             | ✅ §4 — 4 inline + overflow tree      |
| 💎 | Journaling                         | ✅ $LogFile (redo + undo)         | ✅ JBD2 (ordered)            | ✅ CoW (journal-free)         | ✅ §5 — WAL + CoW                     |
| 💎 | Copy-on-Write                      | ❌                                | ❌                           | ✅ Native                     | ✅ §6 — native CoW + refcounts        |
| ⭐ | **Snapshots**                      | ⚠️ VSS (separate service)        | ❌ (LVM only)                | ✅ Native                     | ✅ §6 — filesystem-level, instant   |
| ⭐ | **Auto snapshots**              | ❌ VSS scheduled task             | ❌                           | ⚠️ Manual scripts            | ⬜ §6.2 — configurable retention       |
| 💎 | Per-block checksums                | ❌                                | ✅ CRC32C (metadata only)    | ✅ All data + metadata        | ✅ §7 — CRC32C all blocks             |
| ⭐ | **Self-healing**                | ❌ Requires chkdsk                | ❌ Requires fsck             | ✅ ZFS with mirrors           | ⬜ §7.2 — backup superblock + bitmap   |
| 💎 | Alternate Data Streams             | ✅ Native                         | ❌                           | ❌                            | ⬜ §9 — native via stream inodes      |
| 💎 | Security descriptors (ACLs)        | ✅ Full DACL/SACL                 | ✅ POSIX ACLs (different)    | ✅ POSIX ACLs                | ⬜ §10 — Win32-native DACLs           |
| 💎 | Hard links                         | ✅                                | ✅                           | ✅                            | ⬜ §11.1 — existing `i_links` field   |
| 💎 | Symbolic links                     | ✅ Reparse points                 | ✅ symlink()                 | ✅                            | ⬜ §11.2 — reparse-compatible         |
| ⭐ | **Transparent compression**     | ⚠️ LZ77 (1993 algo, slow)        | ❌                           | ✅ Zstd/LZO                  | ⬜ §12 — LZ4 (fastest) + Zstd         |
| ⭐ | **Per-file encryption**         | ⚠️ EFS (certificate nightmare)   | ✅ fscrypt (CLI setup)       | ✅ ZFS native encryption     | ⬜ §13 — simple master key model      |
| ⭐ | **Inline deduplication**        | ❌                                | ❌                           | ✅ ZFS (needs huge RAM)       | ⬜ §14 — lightweight, uses CoW refs   |
| ⭐ | **Reflinks (instant copy)**     | ⚠️ Server 2016+ only             | ❌                           | ✅ Native                    | ⬜ §15 — transparent via CopyFile     |
| ⭐ | **Online defrag**               | ✅ Windows Defragmenter           | ✅ e4defrag (limited)        | ✅ btrfs defrag              | ⬜ §16 — extent consolidation          |
| ⭐ | **Online resize**               | ✅ Grow only                      | ✅ Grow only (resize2fs)     | ✅ Grow + shrink             | ⬜ §1.2 — grow (shrink future)        |
| 💎 | Inline small files                 | ✅ Resident $DATA                 | ✅ Inline data               | ❌                            | ✅ §4 — ≤48 bytes in `i_extents`      |
| 💎 | Directory hash index               | ✅ B+ tree                        | ✅ HTree (Half MD4)          | ✅ B-tree                    | ✅ §3 — FNV-1a hash index             |
| ⭐ | **Volume health dashboard**     | ❌ Requires chkdsk                | ❌ CLI tune2fs only          | ⚠️ ZFS `zpool status`        | ⬜ §18 — GUI health panel             |
| ⭐ | **Feature count**                  | 11/20                             | 7/20                         | 14/20                        | **20/20** — all features combined   |
