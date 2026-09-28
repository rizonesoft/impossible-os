---
schema_version: 1
id: ext4-readwrite
domain: 05-storage-filesystems
status: active
title: "TODO-09 -- ext4 Read/Write Driver"
---

# TODO-09 -- ext4 Read/Write Driver

> **Goal:** Implement a complete ext4 read/write driver -- superblock + block group descriptors, inode reader, extent tree decoder (with triple-indirect fallback), htree directory B+ tree, file read (including inline data), JBD2 journal replay on mount, file write wrapped in JBD2 transactions, extent tree mutation, directory write, create/delete/rename, extended attributes, and VFS registration with fsck. Windows 11 still cannot natively read ext4; this is a genuine differentiator that lets users with Linux dual-boots or Linux-formatted USB drives access their files.

> [!IMPORTANT]
> ext4 is the default filesystem on every major Linux distribution (Ubuntu, Fedora, Debian, Arch). The existing codebase has only a `probe_ext2_sector2()` magic-byte check in `src/kernel/fs/partition.c` -- this is a blank-slate driver. **JBD2 journal replay (§6) must run before any write path (§7–§10) is active** -- mounting without journal replay risks data corruption on a dirty volume. Refuse the mount if `s_feature_incompat` contains unknown bits; mount read-only if only `s_feature_ro_compat` does. Wire `ext4_probe()` into `vfs_probe()` at step 5 (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1`). This is a `⭐` exclusive feature -- Windows 11 cannot read ext4 natively.

> [!IMPORTANT]
> **From-scratch here is a LICENSING constraint, not an oversight.** The obvious shortcut is `lwext4`, and it is unusable: its `LICENSE` is **GPL-2.0**, verified 2026-08-17, which is incompatible with this project's GPL-3.0-only license. The Linux `fs/ext4` driver is GPL-2.0-only for the same reason. Neither may be vendored, adapted, or translated into this tree.
>
> This note exists so a later vendor-first pass does not "discover" lwext4 and file it as a missed opportunity. It was checked. It is off the table. See CLAUDE.md "Vendor-First Evaluation" and the CREDITS.md "Sources that cannot be used" table.

## Inputs

- `src/kernel/fs/partition.c` -- `probe_ext2_sector2()`, called from `probe_filesystem()`; magic `0xEF53` detection already present; replace with `ext4_probe()` registered via `vfs_probe()` (TODO-03 §1)
- `src/kernel/fs/fat32/` + `src/kernel/fs/ntfs/` -- reference for VFS driver vtable pattern, cluster-chain traversal style, and block-device I/O helper usage; do not share code
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()`, `vfs_fs_driver`, `vfs_node_t` interface all drivers implement
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- `vfs_probe()` calls `ext4_probe()` at step 5 in the probe priority chain; must return `fs_identify_result_t` with label, total bytes, free bytes
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- `CreateFile` on an ext4 volume calls `NtCreateFile` → `vfs_open` → `ext4_ops.finddir`; ensure `ext4_ops` exposes the full 14-entry `vfs_fs_driver` vtable
- → XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §1` -- VFS passes names verbatim; ext4 `finddir()` must handle case-insensitive comparison (ext4 casefold or ASCII fold fallback)

## Outcome

- Superblock parsed; unknown incompatible feature bits refuse the mount, unknown read-only-compatible bits cause a read-only mount; supported features: `EXTENTS`, `64BIT`, `FLEX_BG`, `META_BG`, `LARGE_FILE`, `HTREE_DIR`, `INLINE_DATA`.
- Inode reader handles 256-byte inodes, 64-bit file sizes, `i_extra_isize`.
- Extent tree lookup works for depth 0–4; triple-indirect fallback for `!EXTENTS` inodes.
- Directory htree lookup and linear-scan fallback; `readdir` iterates all entries.
- File read handles fragmented extents, multi-block reads, and inline data.
- JBD2 journal replayed before first write; committed transactions applied atomically.
- File write and block allocation wrapped in JBD2 transactions; extent tree extended correctly.
- Directory add/remove/rename atomic via JBD2.
- Extended attributes read and written for Linux ACL and capability xattrs.
- `ext4_probe()` integrated into `vfs_probe()` chain; `chkdsk D: /ext4` and fsck working.
- Read-only fallback on journal failure; dirty-volume auto-fsck at mount.

## Implementation Order

| ⭐  | Order | Deliverable                                                                           | Depends On                                                    | Status |
| --- | :---: | ------------------------------------------------------------------------------------- | ------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Superblock + block group descriptors -- parse, feature gating, 64-bit counts       | Block device I/O working                                      |  [ ]   |
| ⭐  |   2   | §2 Inode reader -- `ext4_read_inode()`, 64-bit sizes, flags, `i_extra_isize`          | §1 (inode table LBA from group descriptor)                    |  [ ]   |
| ⭐  |   3   | §3 Extent tree decoder -- header, leaf extents, index nodes, triple-indirect fallback | §2 (inode's `i_block` is the tree root)                       |  [ ]   |
| ⭐  |   4   | §4 Directory htree (B+ tree) + linear scan fallback + `readdir`                       | §2 (dir inode), §3 (extent lookup for dir blocks)             |  [ ]   |
| ⭐  |   5   | §5 File read -- extent-mapped data, multi-block, inline data (`INLINE_DATA_FL`)       | §3 (physical block lookup), §2 (inode flags)                  |  [ ]   |
| ⭐  |   6   | §6 JBD2 journal replay -- superblock, descriptor blocks, transaction apply, sequence  | §1 (journal inode number), §2 (read journal inode)            |  [ ]   |
| ⭐  |   7   | §7 File write -- within-block RMW, block alloc, extent append, JBD2 wrap              | §6 (journal must run before writes), §3 (extent tree)         |  [ ]   |
| ⭐  |   8   | §8 Extent tree write -- `ext4_extent_insert`, leaf split, index propagation, remove   | §7 (writes call the extent mutator), §3 (read path proven)    |  [ ]   |
| ⭐  |   9   | §9 Directory write -- `ext4_dir_add`, `ext4_dir_remove`, htree index maintenance      | §8 (dir block alloc uses extent write), §6 (JBD2 wrap)        |  [ ]   |
| ⭐  |  10   | §10 File create / delete / rename -- inode alloc, link count, block free, atomic move | §9 (dir entry write), §6 (JBD2 atomicity)                     |  [ ]   |
| 💎  |  11   | §11 Extended attributes -- inline xattr, xattr block, ACL + capability xattrs         | §2 (inode `i_extra_isize` for inline xattr), §7 (xattr write) |  [ ]   |
| ⭐  |  12   | §12 VFS registration + probe + fsck + `chkdsk /ext4` + read-only fallback             | §1–§11 all complete                                           |  [ ]   |

> §1–§5 (read path), §6 (journal replay), and §7–§10 (write path) are `⭐` exclusive -- Windows 11 has no native ext4 support and cannot read Linux-formatted drives. §11 is `💎` parity -- Linux and macOS both handle xattrs. §12 is `⭐` for the combined in-kernel fsck without an external tool.

---

## 1. Superblock + Block Group Descriptors `[Sonnet]`

Parse the ext4 superblock at offset 1024. Validate magic. Check feature bits: refuse the mount on unknown incompatible bits, mount read-only on unknown read-only-compatible bits. Read the block group descriptor table.

**Files:** `src/kernel/fs/ext4/ext4_core.c` (new), `include/kernel/fs/ext4.h` (new), `include/kernel/fs/ext4_internal.h` (new)

> [!NOTE]
> Superblock location: always at byte offset 1024 from the partition start (sectors 2–3 for 512 B/sector). Magic: `s_magic = 0xEF53`. Key fields: `s_blocks_count_lo`/`s_blocks_count_hi` (64-bit when `INCOMPAT_64BIT` set), `s_log_block_size` (0→1 KiB, 1→2 KiB, 2→4 KiB), `s_inodes_per_group`, `s_inode_size` (128 or 256 bytes), `s_first_data_block` (1 for 1 KiB blocks, 0 otherwise). Block group descriptor table at block `s_first_data_block + 1`. `s_desc_size` = 32 (ext2/3 compat) or 64 bytes (when `INCOMPAT_64BIT`). Required incompat features to support: `EXT4_FEATURE_INCOMPAT_EXTENTS (0x40)`, `EXT4_FEATURE_INCOMPAT_64BIT (0x80)`, `EXT4_FEATURE_INCOMPAT_FLEX_BG (0x200)`, `EXT4_FEATURE_INCOMPAT_META_BG (0x10)`, `EXT4_FEATURE_INCOMPAT_LARGE_FILE (0x8)`, `EXT4_FEATURE_INCOMPAT_HTREE_DIR (0x2)`, `EXT4_FEATURE_INCOMPAT_INLINE_DATA (0x10000)`.

- [ ] `ext4_sb_t` struct: all geometry/count fields, computed: `block_size`, `blocks_per_group`, `inodes_per_group`, `group_count`, `inode_size`, `desc_size`, flags
- [ ] `ext4_sb_parse(dev, &sb)`: read sector at offset 1024; verify magic; check `s_feature_incompat & ~SUPPORTED_INCOMPAT` == 0 -- if nonzero: log, return -ENOTSUP (caller refuses the mount); populate `ext4_sb_t`
  - Unknown `s_feature_ro_compat` bits (`& ~SUPPORTED_RO_COMPAT`) mount read-only instead; unknown INCOMPAT bits never do, because the layout may be misread (kernel.org ext4 superblock docs).
- [ ] `ext4_group_desc_t`: union of 32-byte (ext2/3) and 64-byte (ext4) descriptor; fields: `bg_inode_table_lo`/`hi`, `bg_block_bitmap_lo`/`hi`, `bg_inode_bitmap_lo`/`hi`, `bg_free_blocks_count_lo`/`hi`, `bg_free_inodes_count_lo`/`hi`
- [ ] `ext4_read_group_desc(sb, group_idx, &gd)`: compute LBA of descriptor table entry; read `desc_size` bytes; populate `ext4_group_desc_t`
- [ ] `ext4_block_to_lba(sb, block)`: `(partition_lba_start + block * sectors_per_block)` -- used by all read/write paths
- [ ] `ext4_read_block(sb, block, buf)`: read `block_size` bytes via blkdev; `ext4_write_block(sb, block, buf)` for the write path
- [ ] Log: `[ext4] Mounted %c: blocks=%llu inode_size=%u block_size=%u groups=%u`
- [ ] Commit: `"fs/ext4: superblock parser -- magic, feature gating, block group descriptors, block I/O helpers"`

## 2. Inode Reader `[Sonnet]`

Read a 256-byte ext4 inode. Decode `i_flags`, `i_size` (64-bit), `i_block[15]`, `i_extra_isize`. Handle 128-byte legacy inodes.

**Files:** `src/kernel/fs/ext4/ext4_inode.c` (new)

> [!NOTE]
> Inode location: given inode number `ino` (1-based), `group = (ino - 1) / inodes_per_group`; `local_idx = (ino - 1) % inodes_per_group`; `inode_table_block = gd.bg_inode_table_lo`; `byte_offset = local_idx * inode_size`; read `inode_size` bytes starting at `block_to_lba(inode_table_block) * 512 + byte_offset`. Key `i_flags` bits: `EXT4_EXTENTS_FL (0x80000)`, `EXT4_INLINE_DATA_FL (0x10000000)`, `EXT4_EA_INODE_FL (0x200000)`, `EXT4_INDEX_FL (0x1000)`. `i_size_lo` + `i_size_high` = 64-bit file size (only valid when `LARGE_FILE` or `EXTENTS` incompat feature set). `i_extra_isize`: if `inode_size > 128`, `i_extra_isize` at offset 128 gives number of extra bytes after the standard 128-byte block -- used for `i_crtime`, `i_mtime_extra`, and inline xattrs.

- [ ] `ext4_inode_t` struct: all on-disk fields (128-byte standard block) + extended fields if `inode_size > 128`; computed: `size` (64-bit from `size_lo + size_high`), `mode`, `uid`, `gid`, `nlinks`, `flags`
- [ ] `ext4_read_inode(sb, ino, &inode)`: compute group + local index; read group descriptor; compute inode table LBA + byte offset; read `inode_size` bytes; populate `ext4_inode_t`
- [ ] `ext4_write_inode(sb, ino, &inode)`: write back `inode_size` bytes to same location
- [ ] `ext4_inode_to_vfs_node(sb, ino, &inode)` → `vfs_node_t*`: set `size`, `flags` (dir, file, symlink), `inode_num`; store `ext4_inode_t` as private data
- [ ] Handle `ino == 2` as root directory (always mounted as VFS root)
- [ ] Commit: `"fs/ext4: inode reader -- group lookup, 64-bit size, flags, i_extra_isize, inode→vfs_node"`

## 3. Extent Tree Decoder `[Opus]`

Walk the extent tree rooted at `i_block[0..14]`. Implement `ext4_extent_lookup(inode, logical_block)` → physical block. Add triple-indirect fallback for `!EXTENTS` inodes.

**Files:** `src/kernel/fs/ext4/ext4_extent.c` (new)

> [!NOTE]
> Extent tree root: `i_block` field (60 bytes) holds the root node when `EXT4_EXTENTS_FL` is set. Root `ext4_extent_header` at `i_block[0]`: `eh_magic = 0xF30A`, `eh_entries` (current entries), `eh_max` (max entries in this node), `eh_depth` (0 = leaf, >0 = index). Leaf nodes: `struct ext4_extent { uint32_t ee_block; uint16_t ee_len; uint16_t ee_start_hi; uint32_t ee_start_lo; }` -- `ee_len > 32768` means uninitialized (sparse hole, return zeros). Index nodes: `struct ext4_extent_idx { uint32_t ei_block; uint32_t ei_leaf_lo; uint16_t ei_leaf_hi; uint16_t ei_unused; }`. Algorithm: at each level, binary search index entries where `ei_block <= logical_block`; follow `ei_leaf` to next level block; at leaf, binary search extents for `ee_block <= logical_block < ee_block + ee_len`. Triple-indirect fallback: `i_block[0..11]` = direct blocks, `i_block[12]` = single-indirect, `i_block[13]` = double-indirect, `i_block[14]` = triple-indirect; each indirect block holds `block_size / 4` block numbers.

- [ ] `ext4_extent_header` / `ext4_extent` / `ext4_extent_idx` structs with exact on-disk layout
- [ ] `ext4_extent_lookup(sb, inode, logical_block, &phys_block)`:
  - If `EXT4_EXTENTS_FL`: walk tree from root in `i_block`; at each index level: read block, binary search `ei_block`; at leaf: binary search `ee_block`; if `ee_len > 32768`: sparse → `*phys_block = 0` (return zeros on read)
  - Else: triple-indirect block lookup via `i_block[0..14]`
  - Return 0 on success, -ENOENT if logical block is beyond file size
- [ ] `ext4_extent_find_leaf(sb, header_buf, depth, logical_block, &leaf_block)`: recursive descent for index nodes
- [ ] `ext4_indirect_lookup(sb, inode, logical_block, &phys_block)`: triple-indirect path; read indirect blocks at each level; cache last read indirect block
- [ ] Log on sparse hole: `[ext4] sparse hole at logical block %u in inode %u`
- [ ] Commit: `"fs/ext4: extent tree decoder -- header, leaf lookup, index descent, sparse holes, triple-indirect fallback"`

## 4. Directory htree (B+ Tree) + Readdir `[Opus]`

Walk the htree index for fast name lookup. Fall back to linear scan for small directories. Implement `ext4_readdir()` iterating all `ext4_dir_entry_2` entries.

**Files:** `src/kernel/fs/ext4/ext4_dir.c` (new)

> [!NOTE]
> htree (Hash Tree) directory: `EXT4_INDEX_FL` set in inode flags. Root block (block 0 of the directory's data): `struct dx_root { struct dx_root_info { uint32_t reserved_zero; uint8_t hash_version (0=legacy, 1=half_md4, 2=tea); uint8_t info_length (8); uint8_t indirect_levels (0 or 1); } info; struct dx_entry { uint32_t hash; uint32_t block; } entries[...]; }`. For lookup: compute `ext4_dirhash(name)` using the specified `hash_version`; binary search `dx_root.entries` for the largest hash ≤ target; for `indirect_levels == 1`: read the indicated block (htree leaf index), binary search again; finally read the data block; linear scan `ext4_dir_entry_2` entries for name match. `ext4_dir_entry_2`: `inode (4)`, `rec_len (2)`, `name_len (1)`, `file_type (1)`, `name[255]` -- each entry is 4-byte aligned; `rec_len` includes padding to next entry; `inode == 0` means deleted.

- [ ] `ext4_dirhash_half_md4(name, len)` + `ext4_dirhash_tea(name, len)` + `ext4_dirhash_legacy(name, len)` -- implement all three variants; select via `dx_root.info.hash_version`
- [ ] `ext4_htree_lookup(sb, dir_inode, name, name_len, &result_ino)`: if `EXT4_INDEX_FL`: follow htree; else: linear scan all directory blocks; return inode number or -ENOENT
- [ ] `ext4_dir_entry_scan(block_buf, block_size, name, name_len, &result_ino)`: linear scan a single 4 KiB block for a matching `ext4_dir_entry_2` entry; skip deleted entries (`inode == 0`)
- [ ] `ext4_readdir(sb, dir_inode, callback, ctx)`: walk all data blocks of directory via `ext4_extent_lookup`; for each block: scan all `ext4_dir_entry_2` entries; skip deleted; call `callback(ctx, name, name_len, ino, file_type)` per valid entry
- [ ] VFS `finddir` callback: `ext4_vfs_finddir(vfs_node, name)` → `ext4_htree_lookup()` → `ext4_read_inode()` → `ext4_inode_to_vfs_node()`
- [ ] VFS `readdir` callback: `ext4_vfs_readdir(vfs_node, index)` → `ext4_readdir()` counting to `index`-th entry; return `vfs_dirent`
- [ ] Commit: `"fs/ext4: directory htree -- half_md4/tea hash, htree descent, linear fallback, readdir VFS callbacks"`

## 5. File Read `[Sonnet]`

Read file data by mapping logical blocks to physical blocks via the extent tree. Handle multi-block reads, sparse holes (return zeros), and inline data.

**Files:** `src/kernel/fs/ext4/ext4_io.c` (new)

> [!NOTE]
> Inline data (`EXT4_INLINE_DATA_FL`): if file size ≤ 60 bytes, the file data is stored directly in the 60-byte `i_block` field of the inode (no extent tree, no block allocation). For slightly larger inline data (≤ 60 + `i_extra_isize` bytes), remaining bytes go into the `system.data` xattr in the inline xattr area after the 128-byte inode. Read path: check `EXT4_INLINE_DATA_FL` first -- return from `i_block` (and optionally the inline xattr `system.data`). Otherwise: `logical_block = offset / block_size`; call `ext4_extent_lookup()` for physical block; if `phys_block == 0`: sparse hole, return zeros; else: read `block_size` bytes; handle partial first/last blocks by offsetting within the buffer.

- [ ] `ext4_file_read(sb, inode, offset, buf, len)`:
  - If `EXT4_INLINE_DATA_FL` and `offset + len <= 60`: memcpy from `inode->i_block`; done
  - Else: loop: `lb = offset / block_size`; `ext4_extent_lookup(lb, &pb)`; if `pb == 0`: memset 0 (sparse); else: `ext4_read_block(pb, tmp)`; copy relevant bytes; advance
- [ ] Sparse hole read: `ext4_extent_lookup` returns `phys_block == 0` for uninitialized extents -- fill with zeros, do not read disk
- [ ] `ext4_read_inline_data(inode, offset, buf, len)`: handle case where inline data spills into the `system.data` xattr (call §11 xattr read)
- [ ] VFS `read` callback: `ext4_vfs_read(vfs_node, buf, offset, len)` → `ext4_file_read()`
- [ ] Commit: `"fs/ext4: file read -- extent-mapped, sparse-hole zero-fill, inline data, VFS read callback"`

## 6. JBD2 Journal Replay `[Opus]`

Read the JBD2 journal inode (always inode 8). Replay committed but not yet checkpointed transactions before mounting read-write. Update the journal sequence after replay.

**Files:** `src/kernel/fs/ext4/ext4_journal.c` (new), `include/kernel/fs/ext4_journal.h` (new)

> [!NOTE]
> JBD2 superblock at the first block of the journal inode (inode 8)'s data: `j_magic = 0xC03B3998`, `j_blocktype = JBD2_SUPERBLOCK_V2 (4)`, `j_sequence` (next expected transaction seq), `j_start` (first log block containing active transactions), `j_first` / `j_last` (journal block range). Descriptor block (type 1): `jbd2_block_tag` entries mapping journal block positions to target filesystem blocks. Commit block (type 2): marks all preceding descriptor+data blocks as committed. Replay algorithm: starting at `j_start`, walk journal blocks in circular order; for each descriptor block: read all `jbd2_block_tag` entries; read corresponding journal data blocks; for each: if the transaction sequence ≤ journal's `j_sequence` and the block has not been checkpointed: write the journal data block to the target filesystem block. Stop at a commit block; advance past it; continue until wrap-around or gap. After replay: clear `JOURNAL_HAS_ORPHAN_LIST` flag; set `s_state = EXT4_VALID_FS` in superblock.

- [ ] `jbd2_sb_t` struct: `j_magic`, `j_sequence`, `j_start`, `j_first`, `j_last`, `j_blocksize`, `j_maxlen`
- [ ] `jbd2_read_journal_sb(sb, &jsb)`: read inode 8; read first block of its extent; verify `j_magic`; if not present or `s_journal_inum` == 0: skip replay (no journal → read-only mount)
- [ ] `jbd2_replay(sb, &jsb)`: walk journal from `j_start` in circular order; for each block: read `j_blocktype`; if descriptor (type 1): collect `(journal_block, fs_block)` pairs; if commit (type 2): write all pending pairs to filesystem; if revoke (type 5): remove revoked blocks from pending set; after full scan: log `[ext4] JBD2: replayed %u transactions, %u blocks applied`
- [ ] `jbd2_transaction_start(sb, &txn)`: allocate a new transaction ID; open a journal descriptor block; record start LBA in `jbd2_txn_t`
- [ ] `jbd2_transaction_add_block(txn, fs_block, buf)`: copy block data into next journal position; add tag to descriptor
- [ ] `jbd2_transaction_commit(sb, txn)`: flush all data blocks to journal; write commit block; `fsync` journal area; update `jsb.j_sequence`
- [ ] Mount-time: if `s_state != EXT4_VALID_FS || EXT4_SB_DIRTY`: run `jbd2_replay()`; then set `s_state = EXT4_VALID_FS`; write superblock
- [ ] Commit: `"fs/ext4: JBD2 journal replay -- descriptor/commit/revoke walk, block apply, transaction start/commit"`

## 7. File Write `[Opus]`

Within-block read-modify-write. Block append via `ext4_alloc_block()`. All mutations wrapped in JBD2 transactions. Update `i_size`, `i_mtime`, `i_ctime` on every write.

**Files:** `src/kernel/fs/ext4/ext4_io.c` (extend)

> [!NOTE]
> Block allocation: `ext4_alloc_block(sb, bg_hint, &phys_block)`: scan block bitmap in block group `bg_hint` first (locality); if no free block: scan other groups; set bit in block bitmap; `jbd2_transaction_add_block(txn, bitmap_block, ...)` to journal the bitmap change. Bitmap location: `gd.bg_block_bitmap_lo` gives the bitmap block; it is `block_size` bytes with one bit per block in the group. Block group count: `sb->blocks_per_group`. After allocating a physical block: add a new extent entry via `ext4_extent_insert()` (§8); write zeroed block to disk; then write the actual data.

- [ ] `ext4_file_write(sb, inode, offset, buf, len)`:
  - `jbd2_transaction_start(sb, &txn)`
  - Within existing block: `ext4_extent_lookup(lb, &pb)`; `ext4_read_block(pb, tmp)`; memcpy; `jbd2_transaction_add_block(&txn, pb, tmp)`
  - New block: `ext4_alloc_block(sb, bg_hint, &pb)`; `ext4_extent_insert(sb, inode, lb, pb, 1)` (§8); zero block; write data; add both to txn
  - Update `i_size`, `i_mtime`, `i_ctime`; `jbd2_transaction_add_block(&txn, inode_block, ...)`
  - `jbd2_transaction_commit(sb, txn)`
- [ ] `ext4_file_truncate(sb, inode, new_length)`: if shorter → `ext4_extent_remove()` for tail extents (§8); free physical blocks; update `i_size`; wrap in JBD2 transaction
- [ ] VFS `write` callback: `ext4_vfs_write(vfs_node, buf, offset, len)` → `ext4_file_write()`
- [ ] Commit: `"fs/ext4: file write -- within-block RMW, block alloc, JBD2 transaction wrap, i_size/i_mtime update"`

## 8. Extent Tree Write `[Opus]`

Insert and remove leaf extents. Split full leaves and propagate index to parent. Goal: sequential writes produce single unfragmented extents.

**Files:** `src/kernel/fs/ext4/ext4_extent.c` (extend)

> [!NOTE]
> Leaf split: when a leaf node has `eh_entries == eh_max`, it must be split. Allocate a new filesystem block for the second half of entries; create an index node pointing to both halves. If the parent index node is also full, recursively split. Tree depth increases when the root node (in `i_block`) fills. Root-node capacity: `(60 - sizeof(ext4_extent_header)) / sizeof(ext4_extent_idx)` = 4 index entries or `(60 - sizeof(ext4_extent_header)) / sizeof(ext4_extent)` = 4 leaf extents. Extent merging: when inserting a new extent adjacent to an existing one (same physical run), merge them by extending `ee_len` up to max 32767 blocks; this keeps the tree shallow for sequential writes.

- [ ] `ext4_extent_insert(sb, inode, log_block, phys_block, len)`:
  - Find or create the leaf node for `log_block`; insert new `ext4_extent` in sorted order; if leaf full → `ext4_extent_leaf_split()` → propagate up; try to merge with adjacent extent
  - Write modified leaf (and index) blocks via `jbd2_transaction_add_block()`; write back inode
- [ ] `ext4_extent_leaf_split(sb, inode, leaf_buf, &new_block)`: allocate new block; move upper half of entries; create index entry in parent pointing to both
- [ ] `ext4_extent_remove(sb, inode, log_block, len)`: find containing extent; if partial: shrink `ee_len` or split into two extents; free physical blocks in freed range; free empty leaf/index blocks; write back inode
- [ ] `ext4_free_block(sb, phys_block)`: clear bit in block bitmap; `jbd2_transaction_add_block()` for bitmap; update `bg_free_blocks_count` in group descriptor
- [ ] Commit: `"fs/ext4: extent tree write -- insert with merge, leaf split, index propagation, remove + block free"`

## 9. Directory Write `[Sonnet]`

Add, remove, and maintain directory entries. Maintain htree index when `EXT4_INDEX_FL`. Allocate new directory blocks when current ones are full.

**Files:** `src/kernel/fs/ext4/ext4_dir.c` (extend)

> [!NOTE]
> `ext4_dir_add`: scan directory data blocks for a `rec_len` gap: an entry is a valid insertion point if `actual_len = 8 + name_len (rounded up to 4)` and `entry.rec_len - actual_len >= 8 + new_name_len (rounded up to 4)`; shrink the existing entry's `rec_len` to `actual_len`; write the new entry in the remaining gap with `rec_len = (old_rec_len - actual_len)`. If no gap: allocate a new directory block via `ext4_alloc_block` + `ext4_extent_insert`; write the new entry with `rec_len = block_size`. `ext4_dir_remove`: find entry; merge its `rec_len` into the preceding entry (`prev.rec_len += deleted.rec_len`); if first entry in block: set `inode = 0` (deleted marker). Update htree index entries when `EXT4_INDEX_FL` (or re-initialize htree if tree becomes unbalanced -- this is optional; acceptable to leave htree stale and fall back to linear scan).

- [ ] `ext4_dir_add(sb, dir_inode, name, name_len, child_ino, file_type)`: scan data blocks for gap; if none: alloc new dir block + extend extents; write `ext4_dir_entry_2`; update dir `i_size`; wrap in JBD2 transaction
- [ ] `ext4_dir_remove(sb, dir_inode, name, name_len)`: find entry; merge `rec_len`; write block; JBD2 transaction
- [ ] `ext4_dir_rename(sb, old_dir, old_ino, old_name, new_dir, new_name)`: `ext4_dir_remove(old_dir, old_name)` + `ext4_dir_add(new_dir, new_name, old_ino)` in a single JBD2 transaction for atomicity
- [ ] VFS callbacks: `ext4_vfs_mkdir`, `ext4_vfs_rmdir` -- create/remove directory entry + allocate/free directory inode + add `.` and `..` entries
- [ ] Commit: `"fs/ext4: directory write -- gap-fill add, rec_len merge delete, atomic rename, JBD2 wrapping"`

## 10. File Create / Delete / Rename `[Sonnet]`

Allocate inodes from inode bitmaps. Manage `i_links_count`. Free all blocks and the inode on unlink when link count reaches zero. Atomic rename via JBD2.

**Files:** `src/kernel/fs/ext4/ext4_ops.c` (new)

> [!NOTE]
> Inode allocation: `ext4_alloc_inode(sb, &ino)`: prefer block group of parent directory (locality); scan inode bitmap (`gd.bg_inode_bitmap_lo` block) for first clear bit; set bit; write bitmap back via JBD2; decrement `bg_free_inodes_count` in group descriptor; initialize inode with mode, uid, gid, timestamps, `i_links_count = 1`. Inode free: `ext4_free_inode(sb, ino)`: clear bit in inode bitmap; increment `bg_free_inodes_count`; update superblock `s_free_inodes_count`. Hard link: `ext4_dir_add()` pointing to existing inode + increment `i_links_count`. Delete: `ext4_dir_remove()` + decrement `i_links_count`; if `i_links_count == 0`: free all extents (walk tree, free each physical block), free inode.

- [ ] `ext4_alloc_inode(sb, parent_group, mode, &ino)`: bitmap scan with bg-locality hint; write inode with defaults; return inode number
- [ ] `ext4_free_inode(sb, ino)`: clear bitmap; update group descriptor + superblock free count; all in JBD2 transaction
- [ ] `ext4_create(sb, dir_inode, name, mode, &new_ino)`: `ext4_alloc_inode()` + `ext4_dir_add()` in single JBD2 transaction; set inode mode and timestamps
- [ ] `ext4_unlink(sb, dir_inode, name)`: `ext4_dir_remove()`; decrement `i_links_count`; if 0: walk extent tree + `ext4_free_block()` for each physical block; `ext4_free_inode()`; all in JBD2 transaction
- [ ] `ext4_rename(sb, old_dir, old_name, new_dir, new_name)`: `ext4_dir_rename()` already handles both dirs; here: if `new_name` exists in `new_dir`: call `ext4_unlink()` first (atomically)
- [ ] VFS callbacks: `ext4_vfs_create`, `ext4_vfs_unlink`, `ext4_vfs_rename`, `ext4_vfs_link` → call the above
- [ ] Commit: `"fs/ext4: create/delete/rename -- inode alloc/free, link count, block free on last unlink, JBD2 atomic"`

## 11. Extended Attributes `[Sonnet]`

Read and write inline xattrs (in the extra inode space) and xattr blocks. Support `system.posix_acl_access` and `security.capability` for Linux interoperability.

**Files:** `src/kernel/fs/ext4/ext4_xattr.c` (new)

> [!NOTE]
> Inline xattr location: if `inode_size > 128 + i_extra_isize`, xattr data starts at offset `128 + i_extra_isize` within the inode record; it begins with `EXT4_XATTR_MAGIC = 0xEA020000` at offset 0. External xattr block: if inline space is exhausted, `i_file_acl_lo` points to a block containing the same xattr format. xattr entry: `struct ext4_xattr_entry { uint8_t e_name_len; uint8_t e_name_index; uint16_t e_value_offs; uint32_t e_value_inum; uint32_t e_value_size; uint32_t e_hash; char e_name[e_name_len]; }`. `e_name_index` encodes the namespace: 1=`user.`, 2=`system.posix_acl_access`, 3=`system.posix_acl_default`, 6=`security.`, 7=`system.`, 8=`trusted.`. Value stored after all entries, growing backwards from end of block/inode-area.

- [ ] `ext4_xattr_get(sb, inode, name_index, name, name_len, buf, buf_len)`: check inline xattr area first; then `i_file_acl_lo` xattr block; scan `ext4_xattr_entry` list; copy value to `buf`; return value length or -ENODATA
- [ ] `ext4_xattr_set(sb, ino, &inode, name_index, name, name_len, value, value_len)`: write to inline area if space available; else allocate/extend xattr block; sort entries; update hash; write via JBD2
- [ ] `ext4_xattr_remove(sb, ino, &inode, name_index, name, name_len)`: find and compact xattr entry list
- [ ] `ext4_xattr_list(sb, inode, buf, buf_len)`: enumerate all xattr names (e.g., `user.foo\0system.posix_acl_access\0`)
- [ ] Expose via `NtQueryEaFile` / `NtSetEaFile` stubs (→ XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §9` for EA interface pattern)
- [ ] Commit: `"fs/ext4: extended attributes -- inline xattr, xattr block, ACL + capability namespaces"`

## 12. VFS Registration + Probe + fsck `[Sonnet]`

Register ext4 with `vfs_probe()`. Wire dirty-volume journal replay. Implement `ext4_fsck()`. Add `chkdsk <drive> /ext4`. Mount read-only if journal replay fails or unknown read-only-compatible features are present; refuse the mount on unknown incompatible features.

**Files:** `src/kernel/fs/ext4/ext4_vfs.c` (new), `src/shell/cmd_chkdsk.c` (extend)

> [!NOTE]
> `ext4_probe(blkdev)`: read sector 2 (byte offset 1024); check `s_magic == 0xEF53`. `ext4_mount()`: `ext4_sb_parse()` → if unknown incompat bits: refuse the mount; if unknown ro_compat bits: mount read-only; `jbd2_replay()` → if replay fails: mount read-only + log; scan root inode (2) for VFS root. `ext4_fsck()`: walk all block group descriptors; for each group: read block bitmap + inode bitmap; walk inode table -- for each non-free inode: walk extent tree + count allocated blocks; verify against block bitmap; detect orphan inodes (allocated but not reachable from directory tree); detect cross-linked blocks (same physical block in two different inodes). `ext4_unmount()`: flush all dirty inode/bitmap/journal blocks; write final commit block; clear `MOUNT_FLAGS` in superblock; write `s_state = EXT4_VALID_FS`.

- [ ] `ext4_probe(blkdev_t *dev)` → read sector 2; return 1 if `buf[56..57] == 0xEF53` and `buf[96..99] & INCOMPAT_EXTENTS`, 0 otherwise (distinguish from ext2/ext3 which also use 0xEF53 magic but lack extents)
- [ ] `ext4_mount(blkdev_t *dev, char letter)` → full init: `ext4_sb_parse` → feature check → `jbd2_replay` → VFS root → `vfs_mount()`
- [ ] VFS driver vtable: implement all 14 `vfs_fs_driver` callbacks delegating to `ext4_*` functions from §2–§10
- [ ] `ext4_fsck(vol, fix)`: bitmap vs. extent-walk cross-check; orphan inode detection; if `fix`: clear orphaned inode bitmaps; log `[ext4] fsck: %u errors, %u orphans`
- [ ] Read-only mount: if `s_feature_ro_compat & ~SUPPORTED` ≠ 0 or journal replay fails: set `vol->read_only = 1`; writes return `STATUS_MEDIA_WRITE_PROTECTED` (unknown INCOMPAT bits refuse the mount)
- [ ] `chkdsk D: /ext4` → `ext4_fsck(vol, fix=0)`; `/fix` calls with `fix=1`
- [ ] Add to `vfs_probe()` probe chain at step 5 (after exFAT, before ISO 9660); update `partition.c` to remove old `probe_ext2_sector2()` stub and redirect to `ext4_probe()`
- [ ] Log: `[ext4] Mounted %c: "%s", %llu inodes, %llu blocks, block_size=%u%s`; `%s` = ` [READ-ONLY]` if read-only
- [ ] Commit: `"fs/ext4: VFS registration -- probe, mount, read-only fallback, journal gating, fsck, chkdsk /ext4"`

---

## OS Comparison


| ⭐  | Feature                                                    | 🪟 Win11                                   | 🐧 Linux                                                                       | 🚀 Impossible OS                                                        |
| --- | ---------------------------------------------------------- | ------------------------------------------ | ------------------------------------------------------------------------------ | ----------------------------------------------------------------------- |
| ⭐  | Native ext4 read support                                   | ❌ No native support (requires third-party | ✅ `ext4.ko`; first-class support since 2008                                   | ⬜ §1–§5 -- , §12; read path with                                       |
| ⭐  | Native ext4 write support                                  | ❌ No native support                       | ✅ `ext4.ko`; full R/W with JBD2                                               | ⬜ §6–§10 -- JBD2-wrapped writes, extent tree mutation,                 |
| ⭐  | JBD2 journal replay on mount                               | ❌ N/A -- no ext4 support                  | ✅ `jbd2.ko`; always replays before mounting                                   | ⬜ §6 -- committed-transaction replay, `EXT4_VALID_FS` state management |
| ⭐  | Extent tree B+ tree (depth 0–4) + triple-indirect fallback | ❌ N/A                                     | ✅ `ext4_ext_find_extent()`; depth up to 5;                                    | ⬜ §3 -- , §8; binary search descent,                                   |
| ⭐  | Directory htree (B+ tree) -- half_md4 / TEA hash           | ❌ N/A                                     | ✅ `dx_probe()` in `namei.c`; htree with                                       | ⬜ §4 -- all three hash variants; linear                                |
| ⭐  | Inline data                                                | ❌ N/A                                     | ✅ `EXT4_INLINE_DATA_FL`; `ext4_readpage_inline()` / `ext4_writepage_inline()` | ⬜ §5 -- `i_block` inline read; inline+xattr data                       |
| ⭐  | ext4 fsck                                                  | ❌ N/A                                     | ✅ `e2fsck` (external tool); not in-kernel                                     | ⬜ §12 -- in-kernel fsck; orphan inode detection                        |
| ⭐  | Refuse unknown incompat, read-only on unknown ro_compat    | ❌ N/A                                     | ✅ `ext4_fill_super()` refuses mount on unknown                                | ⬜ §12 -- incompat refuse; ro_compat `[READ-ONLY]`                      |
| 💎  | Extended attributes                                        | ❌ N/A for ext4; NTFS has                  | ✅ `ext4_xattr_get/set()`; posix_acl + security namespaces                     | ⬜ §11 -- inline xattr + xattr block                                    |
| ⭐  | Replace ext2 probe stub with full ext4 VFS driver          | ❌ N/A                                     | ✅ Full driver since 2.6.28 (2008)                                             | ⬜ §12 -- `ext4_probe()` replaces `probe_ext2_sector2()` stub in        |

> **After §1–§12:** Impossible OS becomes one of only two desktop OS kernels (alongside Linux) capable of natively reading and writing ext4 volumes -- surpassing Windows 11 which requires third-party drivers. The in-kernel `ext4_fsck()` callable via `chkdsk /ext4` is a further exclusive -- Linux uses the external `e2fsck` tool which is not available in-kernel.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Unknown feature bits: an unsupported `s_feature_incompat` bit refuses the mount; an unsupported `s_feature_ro_compat` bit mounts read-only and writes return `STATUS_MEDIA_WRITE_PROTECTED`
- [ ] Superblock: mount Linux-formatted ext4 image in QEMU; `[ext4] Mounted D: "..." 4096 blocks, block_size=4096` in serial log
- [ ] Inode reader: `ext4_read_inode(sb, 2)` returns root dir inode; `mode & S_IFDIR`; `size > 0`
- [ ] Extent tree: read a 100 MiB file fragmented into 3 extents; all bytes match the source
- [ ] Inline data: create a 10-byte file on a Linux ext4 volume; read on Impossible OS returns correct bytes (no block I/O, just inode data)
- [ ] htree lookup: directory with 10 000 files; `FindFirstFileW("D:\\bigdir\\target.txt")` returns in < 1 ms (no linear scan)
- [ ] Journal replay: dirty ext4 volume (Linux crashed mid-write); mount on Impossible OS → `[ext4] JBD2: replayed N transactions, M blocks`; file content correct after replay
- [ ] File write + JBD2: write 1 MiB to a new file; unmount; remount; read back; content identical; journal commit log visible
- [ ] Extent tree write: write 64 MiB sequentially to a new file; verify single extent in inode (no fragmentation); `ee_len == 16384` for 4 KiB blocks
- [ ] Create/delete: create 1000 files; delete 500; verify inode bitmap reflects correct free count; `ext4_fsck()` reports 0 errors
- [ ] Rename: `ext4_rename(dir, "a.txt", dir, "b.txt")` → `FindFirstFileW` returns `"b.txt"` only; old name gone; JBD2 log shows single commit for the rename
- [ ] Extended attributes: Linux-written file with `system.posix_acl_access` xattr; `NtQueryEaFile` returns correct ACL bytes
- [ ] fsck: inject a cross-linked block (two inodes referencing same physical block); `chkdsk D: /ext4` → `[ext4] fsck: 1 error: cross-linked block %u`
- [ ] Commit: `"fs/ext4: complete ext4 R/W driver -- superblock, extents, htree, JBD2, write, create, xattrs, fsck"`
