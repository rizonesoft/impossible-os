---
schema_version: 1
id: btrfs-readonly
domain: 05-storage-filesystems
status: active
title: "TODO-10 -- Btrfs Read-Only Driver"
---

# TODO-10 -- Btrfs Read-Only Driver

> **Goal:** Implement a read-only Btrfs driver -- superblock CRC32C verification, B-tree node format, chunk tree logical→physical address translation, generic tree search + walk, root tree enumeration with subvolume discovery, inode reader, extent data decoder with zlib/LZO/Zstd decompression, directory reader, symlinks + xattrs, and VFS registration. Full write support is deferred -- the CoW B-tree write path is ~50 K lines in Linux; read-only delivers 90% of the value and makes NAS drives, Fedora, and openSUSE volumes accessible.

> [!IMPORTANT]
> Btrfs is the default on openSUSE and Fedora Workstation and is ubiquitous on Synology/QNAP NAS devices. No Btrfs code exists anywhere in the repo -- this is a blank-slate driver. The driver is **unconditionally mounted read-only** (`VFS_READONLY`); no write path is ever entered. §1 (superblock) and §2 (B-tree node format) are the mandatory foundation for every other section. §3 (chunk tree) must resolve before any B-tree descent can happen -- all logical addresses in Btrfs are opaque until translated through the chunk map. Wire `btrfs_probe()` into `vfs_probe()` at step 6 (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1`). Neither Windows 11 nor macOS reads Btrfs natively -- this is a `⭐` exclusive.

## Inputs

- `src/kernel/fs/partition.c` -- no existing Btrfs probe; add `btrfs_probe()` call to `vfs_probe()` at step 6 (after ext4)
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()`, `vfs_fs_driver`, `vfs_node_t` interface; mount read-only via `VFS_READONLY` flag
- `include/kernel/kchecksum.h` -- `kcrc32c()` / `kcrc32c_cont()`, the shared CRC32C helper (hardware path when SSE4.2 is present); no ext4 driver exists yet to copy patterns from, so follow `src/kernel/fs/fat32/` for block-device I/O
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- `vfs_probe()` calls `btrfs_probe()` at step 6; must return `fs_identify_result_t` with label (volume label from superblock), total bytes, free bytes
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- `CreateFile` on a Btrfs volume calls `NtCreateFile` → `vfs_open` → `btrfs_ops.finddir`; read-only vtable must still implement `open`, `read`, `finddir`, `readdir`, `stat`, `readlink`
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §9` -- xattr read interface (`NtQueryEaFile`) pattern reused for `btrfs_xattr_get()`

## Outcome

- Superblock at offset 64 KiB parsed; CRC32C verified; backup superblocks checked on primary failure.
- B-tree node format decoded: internal (index) and leaf nodes, `btrfs_header` CRC verified per node.
- Chunk tree loaded into a logical→physical map; SINGLE and RAID1 stripes handled; RAID5/6 rejected.
- Generic `btrfs_tree_search()` and `btrfs_tree_walk()` implement all item lookups.
- Root tree enumerated; all subvolumes listed; FS_TREE (id 5) mounted as the VFS root.
- Inodes read from subvolume trees; `INODE_ITEM` + `INODE_REF` decoded.
- Files read via `EXTENT_DATA` items; inline, regular, and preallocated extents handled; zlib, LZO, Zstd decompressed.
- Directories listed via `DIR_INDEX` items; symlinks read from inline extents.
- xattrs (`security.capability`, `system.posix_acl_*`) read via `XATTR_ITEM`.
- `btrfs_probe()` integrated into `vfs_probe()` chain; volume mounted unconditionally read-only.

## Implementation Order

| ⭐  | Order | Deliverable                                                                           | Depends On                                                     | Status |
| --- | :---: | ------------------------------------------------------------------------------------- | -------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Superblock parse + CRC32C verify + backup superblock fallback                      | Block device I/O working                                       |  [ ]   |
| ⭐  |   2   | §2 B-tree node format -- `btrfs_header`, internal/leaf node decode, per-node CRC      | §1 (`nodesize`, `sectorsize` from superblock)                  |  [ ]   |
| ⭐  |   3   | §3 Chunk tree -- logical→physical map, SINGLE + RAID1 stripes, RAID5/6 reject         | §2 (chunk tree root traversal uses B-tree node format)         |  [ ]   |
| ⭐  |   4   | §4 Tree search + walk -- `btrfs_tree_search`, `btrfs_tree_walk`, binary search        | §3 (all logical addresses need chunk translation before I/O)   |  [ ]   |
| ⭐  |   5   | §5 Root tree + subvolume enumeration -- ROOT_ITEM walk, FS_TREE mount, subvol listing | §4 (tree search applied to root tree)                          |  [ ]   |
| ⭐  |   6   | §6 Inode reader -- `INODE_ITEM`, `INODE_REF`, VFS node construction                   | §5 (subvolume tree root for the inode's home tree)             |  [ ]   |
| ⭐  |   7   | §7 Extent data decoder -- inline, regular, prealloc; zlib/LZO/Zstd decompression      | §6 (inode needed to locate EXTENT_DATA items)                  |  [ ]   |
| ⭐  |   8   | §8 Directory reader -- `DIR_INDEX` walk, `DIR_ITEM` decode, VFS `finddir`/`readdir`   | §6 (dir inode), §4 (tree walk for DIR_INDEX items)             |  [ ]   |
| ⭐  |   9   | §9 Symlinks + xattrs -- inline EXTENT_DATA target, `XATTR_ITEM` read                  | §7 (symlink uses inline extent), §6 (xattr key uses inode num) |  [ ]   |
| ⭐  |  10   | §10 VFS registration + probe + read-only mount + subvol listing                       | §1–§9 all complete                                             |  [ ]   |

> All rows are `⭐` exclusive -- neither Windows 11 nor macOS can natively read Btrfs. Among non-Linux systems, Impossible OS will be one of very few OS kernels capable of reading Btrfs volumes from NAS devices and Linux workstations.

---

## 1. Superblock Parse + CRC32C Verify `[Sonnet]`

Read the primary superblock at byte offset 65 536. Verify CRC32C. Fall back to backup superblocks at 64 MiB and 256 GiB on failure. Populate `btrfs_vol_t`.

**Files:** `src/kernel/fs/btrfs/btrfs_core.c` (new), `include/kernel/fs/btrfs.h` (new), `include/kernel/fs/btrfs_internal.h` (new)

> [!NOTE]
> Superblock location: always at byte offset 65 536 from partition start (sector 128 for 512 B/sector). Magic: `s_magic[8] = "_BHRfS_M"` at offset 64 within the superblock. CRC32C: the first 32 bytes of the superblock are the checksum; compute CRC32C over bytes 32 to `sizeof(btrfs_super_block)` -- must match. Backup superblocks at byte offsets 64 MiB (67 108 864) and 256 GiB (274 877 906 944); try each in order on primary failure. Key fields: `root` (logical addr of root tree), `chunk_root` (logical addr of chunk tree), `log_root` (logical addr of log tree), `generation`, `fsid[16]`, `nodesize` (16 384 typical), `sectorsize` (4 096 typical), `stripesize`, `label[256]` (volume label, null-terminated UTF-8).

- [ ] `btrfs_vol_t` struct: all geometry/address fields; computed: `nodesize`, `sectorsize`, `root_logical`, `chunk_root_logical`, `label[256]`, `fsid[16]`; chunk map pointer (`btrfs_chunk_map_t *chunks`)
- [ ] `btrfs_crc32c(buf, len)` → hardware CRC32C if CPU supports SSE4.2 (`cpuid` check), else software table-based; reused for all node header checks
- [ ] `btrfs_super_parse(dev, &vol)`: read sectors at offset 65 536; verify magic at offset 64 within buffer; compute and compare CRC32C; if fail → try 64 MiB offset; if fail → try 256 GiB offset; if all fail → return -ENOTSUP
- [ ] `btrfs_read_logical(vol, logical, buf, len)` stub (returns -EAGAIN until §3 chunk map is loaded); used to document the interface for §3
- [ ] Log: `[Btrfs] Superblock OK: label="%s" fsid=%02x%02x... nodesize=%u sectorsize=%u`
- [ ] Commit: `"fs/btrfs: superblock parser -- magic, CRC32C verify, backup superblock fallback"`

## 2. B-Tree Node Format `[Sonnet]`

Decode `btrfs_header` (32 bytes). Parse internal nodes (`btrfs_key_ptr` array) and leaf nodes (`btrfs_item` array + item data area). Verify per-node CRC32C.

**Files:** `src/kernel/fs/btrfs/btrfs_btree.c` (new)

> [!NOTE]
> Every tree node is exactly `nodesize` bytes (typically 16 KiB). `struct btrfs_header` (32 bytes at offset 0): `csum[32]` (CRC32C of bytes 32…nodesize-1), `fsid[16]`, `bytenr` (logical address this node occupies; verify matches the address we fetched), `flags`, `chunk_tree_uuid[16]`, `generation`, `owner` (owning tree objectid), `nritems`, `level`. Level 0 = leaf. Internal node (level > 0): after the 32-byte header, `nritems` × `struct btrfs_key_ptr { btrfs_key key; uint64_t blockptr; uint64_t generation; }` (33 bytes each). Leaf node: after header, `nritems` × `struct btrfs_item { btrfs_key key; uint32_t offset; uint32_t size; }` (25 bytes); item data stored at `nodesize - sum(item.size)` growing backwards. `btrfs_key`: `objectid (8)`, `type (1)`, `offset (8)`.

- [ ] `btrfs_node_t` struct: heap-allocated buffer of `nodesize` bytes; parsed header fields; `is_leaf` flag
- [ ] `btrfs_node_read(vol, logical, &node)`: translate logical → physical via chunk map (§3); read `nodesize` bytes; verify `header.bytenr == logical`; verify CRC32C (`btrfs_crc32c(buf+32, nodesize-32)` must match `header.csum`); return node
- [ ] `btrfs_node_item_data(node, item_idx)` → pointer to item's data within the node buffer (for leaf nodes)
- [ ] `btrfs_key_compare(a, b)`: compare `(objectid, type, offset)` lexicographically; returns -1/0/+1; used in all binary searches
- [ ] `btrfs_node_binary_search(node, key)` → index of entry where `key[i] <= target < key[i+1]`; works for both leaf items and internal key_ptrs
- [ ] Commit: `"fs/btrfs: B-tree node format -- header CRC verify, leaf/internal decode, binary search, item data accessor"`

## 3. Chunk Tree -- Logical→Physical Map `[Opus]`

Load the chunk tree. Build an in-memory logical→physical chunk map. Implement `btrfs_logical_to_physical()`. Handle SINGLE and RAID1 stripes; reject RAID5/6.

**Files:** `src/kernel/fs/btrfs/btrfs_chunk.c` (new)

> [!NOTE]
> The chunk tree is special: before it can be read via the normal B-tree path, the superblock itself contains a copy of the chunk tree's bootstrap chunk items in `super.sys_chunk_array` (2 048 bytes). Format: repeated pairs of `(btrfs_key, btrfs_chunk)` packed end-to-end, total length in `super.sys_chunk_array_size`. This bootstraps the first translation. After parsing the sys_chunk_array, read the full chunk tree from `super.chunk_root` using normal B-tree traversal. `struct btrfs_chunk`: `length (8)`, `owner (8)`, `stripe_len (8)`, `type (8)` (flags: `BTRFS_BLOCK_GROUP_SINGLE/RAID0/RAID1/RAID5/RAID6/DUP/RAID10`), `io_align (4)`, `io_width (4)`, `sector_size (4)`, `num_stripes (2)`, `sub_stripes (2)`, followed by `num_stripes` × `struct btrfs_stripe { devid (8); offset (8); devuuid[16]; }`. For SINGLE: use stripe[0]. For RAID1: read from stripe[0] (stripe[1] is the mirror -- fall back to it if stripe[0] I/O fails). For RAID0/5/6/10: log `[Btrfs] RAID%s not supported (read-only safe degraded)` and refuse translation → mount fails cleanly.

- [ ] `btrfs_chunk_entry_t { uint64_t logical; uint64_t length; uint64_t physical; uint64_t devid; }` -- flattened single-stripe representation
- [ ] `btrfs_chunk_map_t { btrfs_chunk_entry_t *entries; uint32_t count; }` -- sorted by `logical`
- [ ] `btrfs_chunk_map_parse_sys_array(vol, super_buf, &map)`: parse `sys_chunk_array` pairs; add entries to map; populate `vol->chunks` with bootstrap entries only
- [ ] `btrfs_chunk_map_load(vol)`: now that bootstrap entries exist, read full chunk tree from `super.chunk_root`; walk all `CHUNK_ITEM` (type `0xE4`, objectid `0x100`) leaf items; add each chunk to map; sort by `logical` (insertion sort sufficient for typical < 1000 entries)
- [ ] `btrfs_logical_to_physical(vol, logical, &devid, &physical)`: binary search chunk map; compute `physical = entry.physical + (logical - entry.logical)`; return -ERANGE if not found
- [ ] Wire into `btrfs_node_read()` (§2): replace stub with real translation
- [ ] Commit: `"fs/btrfs: chunk tree -- sys_chunk_array bootstrap, full chunk map load, logical→physical, RAID1 fallback"`

## 4. Tree Search + Walk `[Sonnet]`

Generic B-tree descent by key. `btrfs_tree_search()` for single-item point lookup. `btrfs_tree_walk()` for range iteration. All subtree I/O goes through the chunk map.

**Files:** `src/kernel/fs/btrfs/btrfs_btree.c` (extend)

> [!NOTE]
> B-tree descent: start at the root logical address; read node; if internal: binary search `key_ptr` array for largest key ≤ target key; follow `blockptr` (a logical address); recurse; if leaf: binary search items for exact key match. Path depth is bounded by tree depth (typically ≤ 4 for normal volumes; ≤ 8 for theoretical max). `btrfs_tree_walk(root_logical, min_key, max_key, callback)`: walk all leaf items with key in `[min_key, max_key]`; cache the current leaf node to avoid re-reading for sequential item access; call `callback(key, item_data, item_size, ctx)` for each match.

- [ ] `btrfs_tree_search(vol, root_logical, key, &item_buf, &item_size)`: descend from `root_logical`; binary search at each level; on leaf: find exact key match; copy item data into `item_buf`; return 0 or -ENOENT
- [ ] `btrfs_tree_walk(vol, root_logical, min_key, callback, ctx)`: descend to leaf containing `min_key`; iterate items on leaf; when exhausted: follow rightmost path upward until a sibling exists; descend into sibling; continue; stop when `item.key > max_key` or end of tree
- [ ] `btrfs_path_t { uint64_t nodes[8]; int slots[8]; int depth; }` -- path context for efficient traversal (avoids re-reading nodes already in path)
- [ ] `btrfs_node_cache_t` -- LRU cache of last 16 nodes (node logical addr → heap buffer); `btrfs_node_read()` checks cache first; evict LRU on miss
- [ ] Commit: `"fs/btrfs: tree search + walk -- B-tree descent, min_key walk, path context, 16-entry node cache"`

## 5. Root Tree + Subvolume Enumeration `[Sonnet]`

Read the root tree from `super.root`. Walk all `ROOT_ITEM` entries. Mount FS_TREE (subvolume id 5) as the VFS root. Expose other subvolumes under `/.btrfs/<name>`.

**Files:** `src/kernel/fs/btrfs/btrfs_root.c` (new)

> [!NOTE]
> Root tree key for a subvolume: `objectid = subvolume_id`, `type = ROOT_ITEM (0x84)`, `offset = 0`. `struct btrfs_root_item`: first 8 bytes contain `bytenr` (logical address of the subvolume's B-tree root). The top-level FS_TREE has objectid = 5 (`BTRFS_FS_TREE_OBJECTID`). Subvolume names: stored as `DIR_ITEM` items in the root directory of the default subvolume (objectid = `BTRFS_ROOT_TREE_DIR_OBJECTID = 6`); key type `DIR_ITEM (0x54)`. Walk root tree with `btrfs_tree_walk(super.root, min_key={1, ROOT_ITEM, 0}, ...)` to enumerate all subvolume ROOT_ITEM entries (objectid >= 256 are user subvolumes). Default subvolume: `super.default_root_objectid`; if 0: use FS_TREE (5).

- [ ] `btrfs_subvol_t { uint64_t objectid; uint64_t root_logical; char name[256]; }` -- per subvolume descriptor
- [ ] `btrfs_root_load(vol)`: read `btrfs_root_item` for objectid 5 (FS_TREE) from root tree; store `vol->fs_tree_root`
- [ ] `btrfs_subvol_enumerate(vol, callback)`: `btrfs_tree_walk` over root tree for all `ROOT_ITEM` keys with objectid ≥ 256; for each: read name via `DIR_ITEM` in objectid-6 directory; call `callback(objectid, root_logical, name)`
- [ ] `btrfs_default_root(vol)`: return `super.default_root_objectid` if nonzero, else FS_TREE (5)
- [ ] Subvolumes exposed as virtual subdirectory `/.btrfs/` in VFS root; each subvol name maps to a VFS node with its own tree root
- [ ] Log: `[Btrfs] %u subvolume(s) found; default root=subvol:%llu`
- [ ] Commit: `"fs/btrfs: root tree + subvolumes -- ROOT_ITEM walk, FS_TREE mount, subvol virtual dir listing"`

## 6. Inode Reader `[Sonnet]`

Read `INODE_ITEM` (type `0x01`) from a subvolume tree. Decode `INODE_REF` for name and parent. Build `vfs_node_t`.

**Files:** `src/kernel/fs/btrfs/btrfs_inode.c` (new)

> [!NOTE]
> Inode key: `objectid = ino`, `type = INODE_ITEM (0x01)`, `offset = 0`. `struct btrfs_inode_item` (160 bytes): `generation`, `transid`, `size (8)`, `nbytes (8)`, `block_group (8)`, `nlink (4)`, `uid (4)`, `gid (4)`, `mode (4)`, `rdev (8)`, `flags (8)`, `sequence (8)`, then 4 × `btrfs_timespec { sec (8); nsec (4); }` (atime, ctime, mtime, otime). `INODE_REF` key: `objectid = ino`, `type = INODE_REF (0x0C)`, `offset = parent_ino`. `struct btrfs_inode_ref`: `index (8)`, `name_len (2)`, `name[name_len]` -- the filename of this inode within the parent directory.

- [ ] `btrfs_inode_t` struct: parsed `btrfs_inode_item` fields; `ino`, `tree_root` (which subvolume this inode belongs to), `name[256]`, `parent_ino`
- [ ] `btrfs_read_inode(vol, tree_root, ino, &inode)`: `btrfs_tree_search(tree_root, {ino, INODE_ITEM, 0})` → decode; `btrfs_tree_search(tree_root, {ino, INODE_REF, 0})` → decode name and parent
- [ ] `btrfs_inode_to_vfs_node(vol, &inode)` → `vfs_node_t*`: set size, mode (dir/file/symlink), `inode_num`, `tree_root`; read-only flag
- [ ] Root inode: `btrfs_read_inode(vol, fs_tree_root, BTRFS_FIRST_FREE_OBJECTID=256)` -- or objectid 256 may not exist; use objectid from `btrfs_root_item.inode.ino` for the subvolume's root dir inode
- [ ] Commit: `"fs/btrfs: inode reader -- INODE_ITEM decode, INODE_REF name+parent, vfs_node construction"`

## 7. Extent Data Decoder `[Opus]`

Walk `EXTENT_DATA` items for a file. Handle inline extents (data in item body), regular extents (physical block + window), and preallocated extents (zeros). Decompress zlib, LZO, and Zstd extents.

**Files:** `src/kernel/fs/btrfs/btrfs_io.c` (new), `src/kernel/fs/btrfs/btrfs_compress.c` (new)

> [!NOTE]
> `EXTENT_DATA` key: `objectid = ino`, `type = EXTENT_DATA (0x6C)`, `offset = file_offset`. `struct btrfs_file_extent_item`: `generation (8)`, `ram_bytes (8)`, `compression (1)` (0=none, 1=zlib, 2=LZO, 3=Zstd), `encryption (1)` (must be 0; encrypted Btrfs is not yet supported), `other_encoding (2)`, `type (1)` (0=INLINE, 1=REGULAR, 2=PREALLOC). For INLINE: item data immediately follows the 21-byte header; `ram_bytes` = uncompressed size; if `compression != 0`: decompress item data. For REGULAR: `disk_bytenr (8)` = logical addr of the on-disk extent, `disk_num_bytes (8)` = compressed size on disk, `offset (8)` = offset within the decompressed extent, `num_bytes (8)` = bytes of decompressed data to use; if `disk_bytenr == 0`: hole (PREALLOC or sparse -- return zeros).

- [ ] `btrfs_file_read(vol, tree_root, ino, file_offset, buf, len)`:
  - `btrfs_tree_walk(tree_root, {ino, EXTENT_DATA, file_offset}, ...)` for all extents covering `[file_offset, file_offset+len)`
  - For each `EXTENT_DATA` item: dispatch on `type` (INLINE/REGULAR/PREALLOC)
  - INLINE: memcpy item data (decompressing if needed) to `buf`
  - REGULAR: `btrfs_logical_to_physical(disk_bytenr)`; read `disk_num_bytes` from device; decompress; copy `[offset, offset+num_bytes)` window to `buf`
  - PREALLOC / hole (`disk_bytenr == 0`): memset zeros
- [ ] `btrfs_decompress_zlib(in, in_len, out, out_len)`: tiny zlib inflate (~800 lines); zlib streams start with 2-byte header `0x78 0x9C` (default compression)
- [ ] `btrfs_decompress_lzo(in, in_len, out, out_len)`: LZO1X-1 decompress; on-disk format: 4-byte LE total size + segments of `(2-byte segment_size, segment_data)`
- [ ] `btrfs_decompress_zstd(in, in_len, out, out_len)`: Zstd single-frame decompression; either embed a minimal Zstd decoder (~2 K lines, MIT, decompression-only) or use a single-file zstd decompressor
- [ ] Encrypted extents (`encryption != 0`): return -ENOTSUP + log `[Btrfs] encrypted extent not supported`
- [ ] VFS `read` callback: `btrfs_vfs_read(vfs_node, buf, offset, len)` → `btrfs_file_read()`
- [ ] Commit: `"fs/btrfs: extent data -- inline/regular/prealloc, zlib/LZO/Zstd decompression, VFS read callback"`

## 8. Directory Reader `[Sonnet]`

Walk `DIR_INDEX` items for fast readdir. Decode `DIR_ITEM` entries for name lookup. Implement VFS `finddir` and `readdir` callbacks.

**Files:** `src/kernel/fs/btrfs/btrfs_dir.c` (new)

> [!NOTE]
> Two directory item types: `DIR_ITEM` (type `0x54`, key offset = CRC32 hash of name) -- used for name lookups; `DIR_INDEX` (type `0x60`, key offset = sequential 64-bit index starting at 2, since `.` = 0 and `..` = 1) -- used for enumeration. Both have the same body: `struct btrfs_dir_item { btrfs_key location; uint64_t transid; uint16_t data_len; uint16_t name_len; uint8_t type (1=file, 2=dir, 7=symlink, etc.); char name[name_len]; char data[data_len]; }`. Name lookup: `btrfs_tree_search(tree_root, {dir_ino, DIR_ITEM, btrfs_name_hash(name)})` -- the key offset is `btrfs_crc32c(name, name_len)`; if multiple names hash to the same value, each is stored as a list within the same item (chained `btrfs_dir_item` entries). Readdir: `btrfs_tree_walk(tree_root, {dir_ino, DIR_INDEX, 2}, ...)` iterates all entries in insertion order.

- [ ] `btrfs_name_hash(name, len)` → `btrfs_crc32c(name, len)` (reuse §1's CRC32C implementation)
- [ ] `btrfs_dir_lookup(vol, tree_root, dir_ino, name, name_len, &result_ino, &result_type)`: `btrfs_tree_search({dir_ino, DIR_ITEM, hash})`; if item found: linear scan chained `btrfs_dir_item` for exact name match; return `location.objectid` and `type`
- [ ] `btrfs_readdir(vol, tree_root, dir_ino, callback, ctx)`: `btrfs_tree_walk` with min_key `{dir_ino, DIR_INDEX, 2}`; for each item: decode `btrfs_dir_item`; call `callback(ctx, name, name_len, child_ino, type)`; stop when objectid ≠ `dir_ino`
- [ ] VFS `finddir` callback: `btrfs_vfs_finddir(vfs_node, name)` → `btrfs_dir_lookup()` → `btrfs_read_inode()` → `btrfs_inode_to_vfs_node()`
  - Name comparison must be case-insensitive (ASCII fold) -- VFS passes names verbatim
- [ ] VFS `readdir` callback: `btrfs_vfs_readdir(vfs_node, index)` → `btrfs_readdir()` counting to `index`-th DIR_INDEX entry; return `vfs_dirent`
- [ ] Commit: `"fs/btrfs: directory reader -- DIR_INDEX walk, DIR_ITEM hash lookup, name chain scan, VFS callbacks"`

## 9. Symlinks + XAttrs `[Sonnet]`

Read symlink targets from inline `EXTENT_DATA`. Read `XATTR_ITEM` entries for `security.capability` and `system.posix_acl_*` Linux interoperability.

**Files:** `src/kernel/fs/btrfs/btrfs_dir.c` (extend), `src/kernel/fs/btrfs/btrfs_xattr.c` (new)

> [!NOTE]
> Symlinks: a symlink inode has `mode & S_IFLNK` and a single inline `EXTENT_DATA` item (type=INLINE, compression=none). The item data is the null-terminated target path string (up to 4 095 bytes). `btrfs_readlink(vol, tree_root, ino)`: `btrfs_tree_search({ino, EXTENT_DATA, 0})`; verify type==INLINE; return item data as string. Xattrs: `XATTR_ITEM` key: `objectid = ino`, `type = XATTR_ITEM (0x18)`, `offset = btrfs_crc32c(name, name_len)`. Body: same `btrfs_dir_item` struct as directory items (name field = xattr name, data field = xattr value). Multiple xattrs with the same hash are chained within the same item.

- [ ] `btrfs_readlink(vol, tree_root, ino, buf, buf_len)`: search `{ino, EXTENT_DATA, 0}`; verify INLINE; copy item data to `buf`; return length or -EINVAL if not symlink
- [ ] `btrfs_xattr_get(vol, tree_root, ino, name, name_len, buf, buf_len)`: `btrfs_tree_search({ino, XATTR_ITEM, hash})`; linear scan chained dir_items for name match; copy `data` field to `buf`; return data length or -ENODATA
- [ ] `btrfs_xattr_list(vol, tree_root, ino, buf, buf_len)`: `btrfs_tree_walk({ino, XATTR_ITEM, 0})`; collect all xattr names into null-separated list
- [ ] VFS `readlink` callback: `btrfs_vfs_readlink(vfs_node, buf, len)` → `btrfs_readlink()`
- [ ] Expose via `NtQueryEaFile` stub (→ XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §9`)
- [ ] Commit: `"fs/btrfs: symlinks + xattrs -- inline extent target, XATTR_ITEM name-hash lookup, VFS readlink"`

## 10. VFS Registration + Probe + Read-Only Mount `[Sonnet]`

Register Btrfs with `vfs_probe()`. Mount unconditionally read-only. Show subvolume count in log. Display "(read-only)" in volume properties.

**Files:** `src/kernel/fs/btrfs/btrfs_vfs.c` (new)

> [!NOTE]
> `btrfs_probe(blkdev)`: read sector at byte offset 65 536 (sector 128 for 512 B/sector); check bytes 64–71 for `"_BHRfS_M"`; return 1 (match) or 0. `btrfs_mount()`: `btrfs_super_parse()` → `btrfs_chunk_map_parse_sys_array()` → `btrfs_chunk_map_load()` → `btrfs_root_load()` → `btrfs_subvol_enumerate()` → `vfs_mount(letter, &btrfs_driver, root_vfs_node, VFS_READONLY)`. Write callbacks: all 6 mutating VFS callbacks (`write`, `create`, `unlink`, `rename`, `mkdir`, `rmdir`) return `STATUS_MEDIA_WRITE_PROTECTED` immediately; never call any internal write function. `btrfs_umount()`: free chunk map, free cached nodes, free subvol list.

- [ ] `btrfs_probe(blkdev_t *dev)` → read at offset 65 536; check `buf[64..71] == "_BHRfS_M"`; return 1 or 0
- [ ] `btrfs_mount(blkdev_t *dev, char letter)` → full init chain; `vfs_mount(letter, ..., VFS_READONLY)`
- [ ] VFS driver vtable: read-only callbacks (`open`, `close`, `read`, `finddir`, `readdir`, `stat`, `readlink`) delegate to §6–§9 functions; write callbacks return `STATUS_MEDIA_WRITE_PROTECTED`
- [ ] `btrfs_umount(vol)`: free `vol->chunks->entries`; free all cached nodes; free subvol list
- [ ] Volume properties: `btrfs_get_label(vol)` → `super.label`; `btrfs_get_free_space(vol)` → scan block group items (BLOCK_GROUP_ITEM type) for `used` vs `length`
- [ ] Add to `vfs_probe()` probe chain at step 6 (after ext4)
- [ ] Log: `[Btrfs] Mounted %c: "%s" (read-only), %u subvol(s), nodesize=%u`
- [ ] Commit: `"fs/btrfs: VFS registration -- probe, read-only mount, subvol listing, write callbacks stub out"`

---

## OS Comparison


| ⭐  | Feature                                              | 🪟 Win11             | 🐧 Linux                                                            | 🚀 Impossible OS                                          |
| --- | ---------------------------------------------------- | -------------------- | ------------------------------------------------------------------- | --------------------------------------------------------- |
| ⭐  | Btrfs superblock + CRC32C verify + backup superblock | ❌ No native support | ✅ `btrfs.ko`; CRC32C hardware-accelerated; 3 backup                | ⬜ §1 -- hardware CRC32C via SSE4.2; 3-level              |
| ⭐  | B-tree node format + per-node CRC verify             | ❌ No native support | ✅ `btrfs.ko`; per-node CRC verified on                             | ⬜ §2 -- `btrfs_key_compare`, binary search, per-node CRC |
| ⭐  | Chunk tree logical→physical + RAID1 stripe fallback  | ❌ No native support | ✅ `btrfs.ko`; all RAID levels; multi-device                        | ⬜ §3 -- SINGLE + RAID1; sys_chunk_array bootstrap        |
| ⭐  | Generic tree search + walk with node cache           | ❌ No native support | ✅ `btrfs.ko`; `btrfs_search_slot()` + `btrfs_next_leaf()`          | ⬜ §4 -- path context, 16-entry node LRU                  |
| ⭐  | Root tree + subvolume enumeration                    | ❌ No native support | ✅ `btrfs.ko`; subvol list via `btrfs                               | ⬜ §5 -- ROOT_ITEM walk; `/.btrfs/` virtual dir           |
| ⭐  | Inode reader                                         | ❌ No native support | ✅ `btrfs.ko`; full inode decode; nanosecond                        | ⬜ §6 -- 160-byte inode item; `otime` decoded             |
| ⭐  | Extent data                                          | ❌ No native support | ✅ `btrfs.ko`; all compression codecs; `ValidDataLength`-equivalent | ⬜ §7 -- self-contained zlib inflate + LZO1X              |
| ⭐  | Directory reader                                     | ❌ No native support | ✅ `btrfs.ko`; `btrfs_lookup_dir_item()` + `btrfs_readdir()`        | ⬜ §8 -- CRC32C name hash, chain scan,                    |
| ⭐  | Symlinks + xattrs                                    | ❌ No native support | ✅ `btrfs.ko`; xattr namespace support; inline                      | ⬜ §9 -- inline EXTENT_DATA symlink; XATTR_ITEM hash      |
| ⭐  | Read-only VFS mount                                  | ❌ No native support | ❌ Linux Btrfs is always R/W                                        | ⬜ §10 -- `VFS_READONLY` flag; all 6 write                |

> **After §1–§10:** Impossible OS joins Linux as one of only two mainstream OS kernels capable of reading Btrfs volumes -- surpassing Windows 11, macOS, FreeBSD, and every other non-Linux OS. The unconditional read-only mount with clean write-protection errors (rather than silently corrupting the volume by attempting a write) is safer than a naive partial-write implementation. The self-contained zlib, LZO, and Zstd decompressors mean no external library dependencies in kernel space.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Probe: Btrfs image → `[Btrfs] Superblock OK`; FAT32 image → `btrfs_probe` returns 0 (no false positive)
- [ ] CRC32C: corrupt 1 byte in the superblock; mount → `[Btrfs] primary superblock CRC mismatch, trying backup`; corrupt all 3 → mount fails with -ENOTSUP
- [ ] Chunk map: `btrfs_logical_to_physical(super.chunk_root)` returns a valid physical address reachable by the dev I/O layer
- [ ] Tree search: `btrfs_tree_search(super.root, {5, ROOT_ITEM, 0})` returns a `btrfs_root_item` whose `bytenr` is a valid logical address
- [ ] Subvolumes: Btrfs volume with 3 user subvolumes; `ls /.btrfs/` shows 3 directory entries; each is readable
- [ ] Directory listing: `FindFirstFileW("D:\\*.*")` on a Btrfs FS_TREE root enumerates all files; filenames match Linux `ls` output
- [ ] File read (uncompressed): read a 4 KiB file; content byte-for-byte matches the Linux-written original
- [ ] File read (zlib): Btrfs volume created with `-o compress=zlib`; read a 1 MiB file; decompressed content correct
- [ ] File read (Zstd): Btrfs volume created with `-o compress=zstd`; read a 1 MiB file; decompressed content correct
- [ ] Sparse file: file with a 1 GiB hole followed by 4 KiB data; hole reads return zeros; data at offset 1 GiB correct
- [ ] Symlink: `btrfs_readlink()` on a symlink inode returns the correct target path
- [ ] Xattr: Linux-written file with `security.capability`; `NtQueryEaFile` returns correct bytes
- [ ] Write protection: `WriteFile` to Btrfs drive → returns `ERROR_WRITE_PROTECT`; no data written; no Btrfs tree modified
- [ ] RAID5/6 reject: Btrfs RAID5 volume → `[Btrfs] RAID5 not supported` in log; mount fails cleanly
- [ ] Commit: `"fs/btrfs: complete read-only Btrfs driver -- superblock, B-tree, chunks, extents, decompression, dirs, xattrs"`
