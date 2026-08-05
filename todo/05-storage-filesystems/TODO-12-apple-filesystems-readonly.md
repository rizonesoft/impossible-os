---
schema_version: 1
id: apple-filesystems-readonly
domain: 05-storage-filesystems
status: active
title: "TODO-12 -- Apple Filesystems: APFS & HFS+ (Read-Only)"
---

# TODO-12 -- Apple Filesystems: APFS & HFS+ (Read-Only)

> **Goal:** Implement read-only drivers for APFS (Apple File System) and HFS+ (Mac OS Extended) -- the two Apple filesystems encountered on external drives shared with macOS users. APFS: container superblock → checkpoint → object map B-tree → volume superblock → filesystem B-tree → inodes, extents, directories, xattrs. HFS+: volume header → journal replay → Catalog B-tree → directory + file read. Both mount unconditionally read-only. GPT partition GUIDs for both already exist in `gpt.c`.

> [!IMPORTANT]
> Write support is deliberately out of scope: APFS write requires Apple's proprietary object map B-tree mutation (partially reverse-engineered only), and HFS+ write requires safe journal mutation -- the complexity is not justified for an interop-only use case. GPT partition type GUIDs `GPT_GUID_APPLE_APFS` and `GPT_GUID_APPLE_HFS` already exist in `src/kernel/fs/gpt.c` -- use these to restrict `apfs_probe()` and `hfsplus_probe()` to Apple-signed partitions. Wire `apfs_probe()` at step 8 and `hfsplus_probe()` at step 9 in `vfs_probe()` (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1`). FileVault-encrypted APFS files must return `STATUS_ACCESS_DENIED` cleanly -- never attempt decryption.

## Inputs

- `src/kernel/fs/gpt.c` + `include/kernel/fs/gpt.h` -- `GPT_GUID_APPLE_APFS` and `GPT_GUID_APPLE_HFS` constants; `gpt_guid_equal()` for partition identification in the probe functions
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()`, `vfs_fs_driver`, read-only mount via `VFS_READONLY`
- `src/kernel/fs/ext4/ext4_core.c` -- reference for CRC64 / block I/O helper pattern; APFS uses Fletcher-64, not CRC32C -- implement separately
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- `vfs_probe()` calls `apfs_probe()` (step 8) then `hfsplus_probe()` (step 9); must return `fs_identify_result_t` with label, total bytes, free bytes
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- `CreateFile` on an Apple volume calls `NtCreateFile` → `vfs_open` → Apple fs vtable; ensure both vtables expose the full read-only 14-entry `vfs_fs_driver`

## Outcome

- APFS container superblock parsed; Fletcher-64 checksum verified; latest checkpoint resolved.
- APFS container object map B-tree loaded; virtual OID → physical block translation working.
- APFS volume superblock parsed; filesystem B-tree searched; volumes enumerated.
- APFS inodes read via `j_inode_val_t`; data stream size, timestamps, mode extracted.
- APFS files read via `j_file_extent_val_t`; FileVault-encrypted extents return `STATUS_ACCESS_DENIED`.
- APFS directories walked via `j_drec_hashed_key_t`; NFD→NFC normalization applied for display.
- APFS symlinks and xattrs read via `j_xattr_val_t`.
- HFS+ volume header parsed; journal replayed before read-only mount; fork data decoded.
- HFS+ Catalog B-tree searched; directory listing and file read working; `HFSUniStr255` decoded.
- HFS+ fragmented files handled via Extents Overflow B-tree.
- Both `apfs_probe()` and `hfsplus_probe()` integrated into `vfs_probe()`; display "(read-only, Apple)".

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On                                                             | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 APFS container superblock -- `BSXN` magic, Fletcher-64, checkpoint resolution         | Block device I/O                                                       |  [ ]   |
| ⭐  |   2   | §2 APFS object map B-tree -- `omap_lookup()`, virtual OID → physical block               | §1 (`nx_omap_oid` from container superblock)                           |  [ ]   |
| ⭐  |   3   | §3 APFS volume mount -- `APSB` volume superblock, volume enumeration, fstree root        | §2 (omap needed to resolve volume OIDs from `nx_fs_oid[]`)             |  [ ]   |
| ⭐  |   4   | §4 APFS filesystem B-tree search + walk -- `j_key_t` type-dispatch, range walk           | §3 (fstree root logical address from volume superblock)                |  [ ]   |
| ⭐  |   5   | §5 APFS inode reader -- `j_inode_val_t`, extended fields, dstream, VFS node              | §4 (INODE_TYPE items found via fstree search)                          |  [ ]   |
| ⭐  |   6   | §6 APFS file extent reader -- extent walk, physical read, FileVault reject               | §5 (inode's private_id used as FILE_EXTENT key), §4 (fstree walk)      |  [ ]   |
| ⭐  |   7   | §7 APFS directory reader -- `j_drec_hashed_key_t`, NFD→NFC, VFS callbacks               | §5 (dir inode), §4 (DIR_REC items via fstree walk)                     |  [ ]   |
| ⭐  |   8   | §8 APFS symlinks + xattrs -- `j_xattr_val_t`, inline + stream, symlink target           | §4 (XATTR_TYPE items), §5 (inode for key construction)                 |  [ ]   |
| ⭐  |   9   | §9 HFS+ volume header -- `H+`/`HX` magic, fork data, journal replay, block layout       | Block device I/O                                                       |  [ ]   |
| ⭐  |  10   | §10 HFS+ Catalog B-tree -- header node, leaf search, `HFSUniStr255` decode, readdir      | §9 (Catalog File fork data = B-tree root block)                        |  [ ]   |
| ⭐  |  11   | §11 HFS+ file read + Extents Overflow B-tree -- inline extents, overflow lookup          | §10 (Catalog record gives inline extents + CNID for overflow lookup)   |  [ ]   |
| ⭐  |  12   | §12 VFS registration -- `apfs_probe`, `hfsplus_probe`, read-only mount, volume metadata  | §1–§11 complete                                                        |  [ ]   |

> All rows are `⭐` exclusive -- neither Windows 11 nor any non-Apple, non-Linux OS reads APFS or HFS+ natively. Linux reads HFS+ via `hfsplus.ko` but has no upstream APFS driver (only the third-party `apfs-fuse` in userspace). Impossible OS will deliver native in-kernel APFS read alongside HFS+ read in a single driver pair.

---

## 1. APFS Container Superblock + Fletcher-64 `[Sonnet]`

Read block 0 of the partition. Verify the `obj_phys_t` header checksum using Apple's Fletcher-64 variant. Locate the latest valid checkpoint and read the container superblock.

**Files:** `src/kernel/fs/apfs/apfs_core.c` (new), `include/kernel/fs/apfs.h` (new), `include/kernel/fs/apfs_internal.h` (new)

> [!NOTE]
> Every APFS on-disk object has a 32-byte `obj_phys_t` header: `o_cksum[8]` (Fletcher-64 checksum of bytes 8 onwards), `o_oid[8]` (object identifier), `o_xid[8]` (transaction ID), `o_type[4]` (type + flags), `o_subtype[4]`. Fletcher-64 algorithm (Apple variant): split the buffer starting at offset 8 into 4-byte words; two running sums `f1` and `f2`; `f1 += word; f2 += f1;` for each word; final checksum = `((~f2 & 0xFFFFFFFF) << 32) | (~f1 & 0xFFFFFFFF)`. Container superblock: block 0, `o_type = OBJECT_TYPE_NX_SUPERBLOCK (0xED)`, magic `0x4253584E` (`BSXN`). Key fields: `nx_block_size` (4096 typical), `nx_block_count`, `nx_xp_desc_base` (checkpoint descriptor area start block), `nx_xp_desc_blocks` (count), `nx_omap_oid` (container object map OID), `nx_fs_oid[100]` (volume OIDs, 0 = unused slot). Checkpoint: the checkpoint descriptor area contains `nx_xp_desc_blocks` checkpoint map entries; the latest valid checkpoint has the highest `xid`; read its `cpm_paddr` (physical block) to get the validated container superblock.

- [ ] `apfs_fletcher64(buf, len)` → `uint64_t`: Apple's specific variant; exclude the first 8 bytes (checksum field itself)
- [ ] `apfs_obj_verify(buf, block_size)`: compute Fletcher-64 of `buf[8..block_size-1]`; compare to `obj_phys_t.o_cksum`; return 0 or -EBADMSG
- [ ] `apfs_vol_t` struct (container context): `block_size`, `block_count`, `nx_omap_oid`, `fs_oid[100]`, `xp_desc_base`, `xp_desc_blocks`, `blkdev`
- [ ] `apfs_container_parse(blkdev, &vol)`: read block 0; verify `BSXN` magic; call `apfs_obj_verify()`; if fail → scan checkpoint descriptor area for latest valid checkpoint; read that block as container superblock; populate `apfs_vol_t`
- [ ] `apfs_read_block(vol, paddr, buf)`: read one `block_size` block at `paddr * block_size`; verify checksum; return buffer
- [ ] Log: `[APFS] Container: block_size=%u blocks=%llu fs_count=%u xid=%llu`
- [ ] Commit: `"fs/apfs: container superblock -- BSXN magic, Fletcher-64, checkpoint resolution, block read+verify"`

## 2. APFS Object Map B-Tree `[Opus]`

Read the container object map. Walk `btree_node_phys_t` nodes. Implement `apfs_omap_lookup(oid, xid)` → physical block address. Cache the omap root node.

**Files:** `src/kernel/fs/apfs/apfs_omap.c` (new)

> [!NOTE]
> The object map B-tree root is at the physical block given by reading the OID in `nx_omap_oid` from the container superblock: `apfs_read_block(vol, paddr=apfs_omap_lookup_phys_from_omap_oid(...))`. But the omap OID is itself a *physical* address stored as an ephemeral object (type flags `OBJ_EPHEMERAL`) in the checkpoint -- it can be resolved directly without a recursive omap lookup. B-tree node: `btree_node_phys_t` header (32 bytes) followed by table-of-contents (TOC) area + key area + free area + value area. TOC entry: `kvoff_t { key_off (uint16), val_off (uint16) }` for leaf nodes; the key and value areas grow toward each other from each end of the node's data space. Object map leaf entry: `omap_key_t { ok_oid (8), ok_xid (8) }` → `omap_val_t { ov_flags (4), ov_size (4), ov_paddr (8) }`. B-tree search: at each level, binary search TOC entries by key; for internal nodes, follow `ov_paddr` to child block; for leaf nodes, return value. MVCC lookup: find the entry with the highest `ok_xid ≤ target_xid` for the given `ok_oid`.

- [ ] `btree_node_phys_t` + `kvoff_t` + `omap_key_t` + `omap_val_t` struct definitions (exact on-disk layout)
- [ ] `apfs_btree_node_read(vol, paddr, &node_buf)`: `apfs_read_block()` + `apfs_obj_verify()`; return allocated node buffer
- [ ] `apfs_btree_search(vol, root_paddr, key_buf, key_len, &val_buf, &val_len)`: descend B-tree; binary search TOC at each level; for leaf: find matching key; return value bytes
- [ ] `apfs_omap_lookup(vol, omap_root_paddr, oid, xid, &paddr_out)`: build `omap_key_t`; `apfs_btree_search()` with MVCC semantics (highest xid ≤ target); extract `ov_paddr`; return 0 or -ENOENT
- [ ] `apfs_resolve_oid(vol, oid, &paddr)`: convenience wrapper calling `apfs_omap_lookup()` with the container's current `max_xid`
- [ ] Node cache: LRU cache of last 32 nodes (paddr → buffer); `apfs_btree_node_read()` checks cache first
- [ ] Commit: `"fs/apfs: object map B-tree -- btree_node descent, omap MVCC lookup, OID→paddr resolution, node cache"`

## 3. APFS Volume Mount `[Sonnet]`

Resolve each non-zero `nx_fs_oid` to its volume superblock (`APSB`). Parse volume name, size, and fstree root OID. Mount the first non-sealed volume as the VFS root.

**Files:** `src/kernel/fs/apfs/apfs_vol.c` (new)

> [!NOTE]
> Volume superblock: OID from `nx_fs_oid[i]` → physical block via container omap (`apfs_resolve_oid`); object type `OBJECT_TYPE_FS (0x0D)`, magic `0x42535041` (`APSB`). Key fields: `apfs_omap_oid` (volume's own object map OID -- separate from the container's omap; used to resolve virtual filesystem objects within this volume), `apfs_root_tree_oid` (OID of the filesystem B-tree root), `apfs_volname[256]` (UTF-8 volume name), `apfs_num_files`, `apfs_num_directories`, `apfs_fs_flags` -- check `APFS_FS_SEALED (bit 3)`: sealed volumes (Catalina+ system volume) are read-only snapshots; skip and try next volume. Volume omap OID is also an ephemeral object resolved from the volume's checkpoint, not the container's omap.

- [ ] `apfs_fsvol_t` struct: `vol_omap_root_paddr`, `fstree_root_oid`, `label[256]`, `num_files`, `num_dirs`, `is_sealed`; pointer to parent `apfs_vol_t`
- [ ] `apfs_mount_volume(vol, fs_oid, &fsvol)`: `apfs_resolve_oid(fs_oid)` → read `APSB` block; verify magic; check sealed flag; resolve `apfs_omap_oid` via container omap; load volume omap root; store `fstree_root_oid`
- [ ] `apfs_enumerate_volumes(vol, callback)`: iterate `nx_fs_oid[0..99]`; skip zeros; for each: `apfs_mount_volume()`; call `callback(fsvol)`; first non-sealed volume becomes the VFS root
- [ ] `apfs_fsvol_resolve_oid(fsvol, oid, &paddr)`: use the volume's own omap to resolve filesystem object OIDs (distinct from container omap)
- [ ] Log: `[APFS] Volume "%s": files=%llu dirs=%llu%s` where `%s` = ` [sealed, skipped]` or empty
- [ ] Commit: `"fs/apfs: volume mount -- APSB parse, volume omap, non-sealed selection, multi-volume enumeration"`

## 4. APFS Filesystem B-Tree Search + Walk `[Opus]`

Implement generic filesystem B-tree search and range walk over the volume's fstree. Dispatch on `j_key_t` type field. Implement `apfs_fstree_walk()` for sequential directory and extent enumeration.

**Files:** `src/kernel/fs/apfs/apfs_fstree.c` (new)

> [!NOTE]
> Filesystem B-tree key: `j_key_t` -- the upper 60 bits of the first 8-byte word are the object ID; the lower 4 bits are the item type: `APFS_TYPE_INODE (0x3)`, `APFS_TYPE_DIR_REC (0x9)`, `APFS_TYPE_FILE_EXTENT (0x8)`, `APFS_TYPE_XATTR (0x4)`, `APFS_TYPE_SIBLING_LINK (0xB)`. Full key comparison: first compare by the 64-bit key header (oid in upper 60 bits, type in lower 4); for equal headers, compare type-specific trailing bytes. B-tree node format: same `btree_node_phys_t` as the omap. Virtual addresses: fstree node OIDs are virtual -- resolve each via `apfs_fsvol_resolve_oid()` before reading the block. Root node: resolve `fstree_root_oid` from the volume superblock.

- [ ] `apfs_fstree_key_compare(a, b)`: compare `j_key_t` header (oid | type) then type-specific suffix bytes; return -1/0/+1
- [ ] `apfs_fstree_search(fsvol, oid, type, suffix_key, suffix_len, &val_buf, &val_len)`: construct `j_key_t`; descend fstree B-tree via `apfs_btree_search()` with filesystem key comparator; resolve each node OID via volume omap; return value bytes
- [ ] `apfs_fstree_walk(fsvol, min_oid, min_type, callback, ctx)`: descend to leaf containing `(min_oid, min_type)`; iterate items; for each: decode `j_key_t` header; call `callback(oid, type, key_suffix, key_suffix_len, val, val_len, ctx)`; stop on end of tree or type/OID change based on caller's predicate
- [ ] `apfs_fstree_node_read(fsvol, oid, &node_buf)`: `apfs_fsvol_resolve_oid(oid)` → `apfs_btree_node_read()`; verify object type `OBJECT_TYPE_FSTREE (0x000E)`
- [ ] Commit: `"fs/apfs: filesystem B-tree -- j_key_t dispatch, virtual OID resolution, search + range walk"`

## 5. APFS Inode Reader `[Sonnet]`

Read `APFS_TYPE_INODE` items from the fstree. Decode `j_inode_val_t` including variable-length extended fields. Extract `j_dstream_t` for file size. Build VFS nodes.

**Files:** `src/kernel/fs/apfs/apfs_inode.c` (new)

> [!NOTE]
> Inode value: `j_inode_val_t`: `parent_id (8)`, `private_id (8)` (used as key for file extents), `create_time (8)`, `mod_time (8)`, `change_time (8)`, `access_time (8)` (nanoseconds since Unix epoch 1970-01-01), `internal_flags (8)`, `nchildren (4)` (dirs) or `nlink (4)` (files), `default_protection_class (4)`, `write_generation_counter (4)`, `bsd_flags (4)`, `owner (4)`, `group (4)`, `mode (2)`, `pad1 (2)`, `uncompressed_size (8)`. Extended fields follow as a blob: `j_inode_val_t.xf_blob_size` bytes of `xf_blob_t` containing a list of `j_xf_t` entries -- each `j_xf_t` has `x_type (1)`, `x_flags (1)`, `x_size (2)` followed by data. Key extended field types: `INO_EXT_TYPE_NAME (0x4)` (original filename string for hard links), `INO_EXT_TYPE_DSTREAM (0xA)` (`j_dstream_t { size (8), alloced_size (8), default_crypto_id (8), total_bytes_written (8), total_bytes_read (8) }`).

- [ ] `apfs_inode_t` struct: all `j_inode_val_t` fields; extracted `dstream.size`, `dstream.crypto_id`, `name_override[256]` (from INO_EXT_TYPE_NAME xf), `private_id`
- [ ] `apfs_read_inode(fsvol, ino, &inode)`: `apfs_fstree_search(ino, APFS_TYPE_INODE, ...)`; decode `j_inode_val_t`; parse xf_blob for INO_EXT_TYPE_NAME and INO_EXT_TYPE_DSTREAM
- [ ] `apfs_xf_parse(xf_buf, xf_size, type, &out_buf, &out_size)`: scan xf entries; return pointer to requested type's data
- [ ] `apfs_inode_to_vfs_node(fsvol, ino, &inode)` → `vfs_node_t*`: set size from dstream, mode (dir/file/symlink), inode_num; read-only flag; store `private_id` for extent lookups
- [ ] APFS epoch: timestamps are nanoseconds since Unix epoch (1970-01-01 UTC); convert to `FILETIME` (100 ns since 1601-01-01): `filetime = (nsec / 100) + 116444736000000000ULL`
- [ ] Commit: `"fs/apfs: inode reader -- j_inode_val_t, xf_blob decode, dstream size, nanosec→FILETIME, VFS node"`

## 6. APFS File Extent Reader `[Sonnet]`

Walk `APFS_TYPE_FILE_EXTENT` items for a file's `private_id`. Map logical offsets to physical blocks. Detect FileVault-encrypted extents and return `STATUS_ACCESS_DENIED`.

**Files:** `src/kernel/fs/apfs/apfs_io.c` (new)

> [!NOTE]
> File extent key: `j_file_extent_key_t { j_key_t hdr (oid=private_id, type=FILE_EXTENT); uint64_t logical_addr; }`. File extent value: `j_file_extent_val_t { uint64_t len_and_flags; uint64_t phys_block_num; uint64_t crypto_id; }`. `len_and_flags`: low 56 bits = length in bytes; high 8 bits = flags. `phys_block_num`: physical block number (not a virtual OID -- direct physical address). Sparse extent: `phys_block_num == 0` and flags bit `0x01` (APFS_FILE_EXTENT_FLAG_UNALLOCATED) → return zeros. FileVault: `crypto_id != 0` → encrypted by FileVault. The `default_crypto_id` in `j_dstream_t` is set for volumes under FileVault; individual extent `crypto_id` may differ.

- [ ] `apfs_file_read(fsvol, inode, offset, buf, len)`:
  - Walk FILE_EXTENT items via `apfs_fstree_walk(private_id, APFS_TYPE_FILE_EXTENT, ...)`
  - For each extent covering `[offset, offset+len)`: if `crypto_id != 0` → return `STATUS_ACCESS_DENIED` (log `[APFS] FileVault encrypted file -- access denied`)
  - If `phys_block_num == 0` (sparse): memset zero; else: `blkdev_read(vol->blkdev, phys_block_num * block_size, ...)` for the relevant bytes
- [ ] `apfs_file_read_extent(vol, phys_block, byte_offset_within, buf, len)`: raw block read; no OID resolution (physical address)
- [ ] VFS `read` callback: `apfs_vfs_read(vfs_node, buf, offset, len)` → `apfs_file_read()`
- [ ] Commit: `"fs/apfs: file extent reader -- FILE_EXTENT walk, physical block read, sparse zeros, FileVault deny"`

## 7. APFS Directory Reader `[Sonnet]`

Walk `APFS_TYPE_DIR_REC` items in the fstree for a given directory OID. Decode hashed keys. Apply NFD→NFC normalization for display. Implement VFS `finddir` and `readdir`.

**Files:** `src/kernel/fs/apfs/apfs_dir.c` (new)

> [!NOTE]
> Directory record key: `j_drec_hashed_key_t`: first 8 bytes = `j_key_t` (oid=parent_dir_oid, type=DIR_REC); then `name_len_and_hash (4)` (low 10 bits = name length including null, high 22 bits = CRC32c hash of the lowercase UTF-8 name); then `name[name_len]` (null-terminated UTF-8). Directory record value: `j_drec_val_t`: `file_id (8)` (child inode OID), `date_added (8)` (nanoseconds since Unix epoch), `flags (2)` (file type in low 4 bits: `DT_REG=4`, `DT_DIR=8`, `DT_LNK=10`, `DT_CHR=2`, `DT_BLK=6`, `DT_FIFO=1`, `DT_SOCK=12`, `DT_WHT=14`), `xf_blob` (optional extended fields). Lookup: to find a file by name, compute `(CRC32c_lowercase(name) & 0x3FFFFF) << 10 | (strlen(name)+1)` for the hash+len field; `apfs_fstree_search()` with the full `j_drec_hashed_key_t`. NFD→NFC: APFS stores filenames in Unicode NFD (decomposed) form; display to the user in NFC (composed) form to match Windows conventions.

- [ ] `apfs_drec_name_hash(name_utf8)` → `uint32_t`: `CRC32c(lowercase_utf8_name)` masked to 22 bits, shifted left 10, ORed with `(strlen+1)`
- [ ] `apfs_nfd_to_nfc(nfd_utf8, len, nfc_out, out_max)`: Unicode canonical decomposition→composition; handle common combining characters (accented Latin, Korean Hangul jamo); for unsupported ranges, pass through unchanged
- [ ] `apfs_dir_lookup(fsvol, dir_oid, name_utf8, &result_oid, &result_type)`: build `j_drec_hashed_key_t`; `apfs_fstree_search()`; extract `file_id` and `flags` file type
- [ ] `apfs_readdir(fsvol, dir_oid, callback, ctx)`: `apfs_fstree_walk(dir_oid, APFS_TYPE_DIR_REC, ...)`; for each item: extract name from key, apply NFD→NFC, extract `file_id`; call `callback(ctx, name, name_len, child_oid, type)`; stop when key OID ≠ `dir_oid`
- [ ] VFS `finddir` callback: `apfs_vfs_finddir(vfs_node, name)` → `apfs_dir_lookup()` → `apfs_read_inode()` → `apfs_inode_to_vfs_node()`
  - Name comparison must be case-insensitive (APFS uses NFD normalization + case-fold) -- VFS passes names verbatim
- [ ] VFS `readdir` callback: `apfs_vfs_readdir(vfs_node, index)` → `apfs_readdir()` counting to `index`-th entry; return `vfs_dirent`
- [ ] Commit: `"fs/apfs: directory reader -- j_drec_hashed_key, CRC32c hash, NFD→NFC normalization, VFS callbacks"`

## 8. APFS Symlinks + XAttrs `[Sonnet]`

Read symlink targets from `APFS_TYPE_XATTR` items using the `com.apple.fs.symlink` xattr name. Read arbitrary xattrs -- inline data or stream dnode. Implement VFS `readlink`.

**Files:** `src/kernel/fs/apfs/apfs_xattr.c` (new)

> [!NOTE]
> Xattr key: `j_xattr_key_t { j_key_t hdr (oid=ino, type=XATTR_TYPE); uint16_t name_len; char name[name_len]; }`. Xattr value: `j_xattr_val_t { uint16_t flags; uint16_t xdata_len; uint8_t xdata[xdata_len]; }`. Flags: `XATTR_DATA_EMBEDDED (0x2)` -- data is inline in `xdata`; `XATTR_DATA_STREAM (0x1)` -- `xdata` is a `j_xattr_dstream_t { uint64_t xattr_obj_id; j_dstream_t dstream; }` where `xattr_obj_id` is a virtual OID for an xattr dnode whose FILE_EXTENT items hold the data (use `apfs_fstree_walk` with the xattr_obj_id). Symlink xattr name: `"com.apple.fs.symlink"` -- inline string; target path is null-terminated UTF-8 in `xdata`. Common xattrs: `"com.apple.quarantine"`, `"security.capability"`, resource fork `"com.apple.ResourceFork"`.

- [ ] `apfs_xattr_get(fsvol, ino, name, buf, buf_len)`: build `j_xattr_key_t`; `apfs_fstree_search()`; decode `j_xattr_val_t`; if `XATTR_DATA_EMBEDDED`: memcpy `xdata`; if `XATTR_DATA_STREAM`: read xattr dnode via `apfs_file_read(xattr_obj_id, ...)`
- [ ] `apfs_xattr_list(fsvol, ino, buf, buf_len)`: `apfs_fstree_walk(ino, APFS_TYPE_XATTR, ...)`; collect all xattr names into null-separated list
- [ ] `apfs_readlink(fsvol, ino, buf, buf_len)`: `apfs_xattr_get(ino, "com.apple.fs.symlink", buf, buf_len)` → null-terminated UTF-8 target
- [ ] VFS `readlink` callback: `apfs_vfs_readlink(vfs_node, buf, len)` → `apfs_readlink()`
- [ ] Commit: `"fs/apfs: symlinks + xattrs -- j_xattr_key, inline + stream, com.apple.fs.symlink, VFS readlink"`

## 9. HFS+ Volume Header + Journal Replay `[Sonnet]`

Read the HFS+ volume header at partition byte offset 1024. Replay the journal if dirty. Decode fork data for the Catalog and Extents Overflow files.

**Files:** `src/kernel/fs/hfsplus/hfsplus_core.c` (new), `include/kernel/fs/hfsplus.h` (new)

> [!NOTE]
> Volume header: `struct HFSPlusVolumeHeader` at byte offset 1024 from the partition start. Magic: `0x482B` (`H+`) or `0x4858` (`HX` = HFSX, case-sensitive variant). Fields: `signature (2)`, `version (2)` (4 = HFS+ 1.x, 5 = HFSX), `attributes (4)` -- bit 2 `kHFSVolumeUnmountedBit` (0 = dirty), bit 5 `kHFSVolumeJournaledBit` (journaling active), `createDate (4)` (seconds since 1904-01-01 00:00:00 UTC), `modifyDate (4)`, `blockSize (4)`, `totalBlocks (4)`, `freeBlocks (4)`, `encodingsBitmap (8)` (which Mac text encodings are used on this volume), Catalog File fork data (`HFSPlusForkData`), Extents Overflow fork data, Hot Files fork data. `HFSPlusForkData`: `logicalSize (8)`, `clumpSize (4)`, `totalBlocks (4)`, `extents[8]` (array of `HFSPlusExtentDescriptor { startBlock (4), blockCount (4) }`). Journal info block: if `kHFSVolumeJournaledBit` set and `kHFSVolumeUnmountedBit` clear: volume is dirty; journal location from `journalInfoBlock` field in volume header; replay before read-only mount.

- [ ] `hfsplus_vol_t` struct: `block_size`, `total_blocks`, `free_blocks`, `is_hfsx`, `catalog_fork`, `extents_fork`, `root_cnid (2)` (always CNID 2 in HFS+)
- [ ] `hfsplus_vhdr_parse(blkdev, &vol)`: read 512 bytes at offset 1024; verify magic; populate `hfsplus_vol_t`; check `kHFSVolumeJournaledBit`
- [ ] `hfsplus_lba(vol, block)` → byte offset: `block * vol->block_size`; used by all reads
- [ ] `hfsplus_journal_replay(vol)`: read journal info block; if journal superblock `JRNL_MAGIC (0x4A4E4C78)` present and `s_start != s_end` (not empty): apply all committed journal transactions (simple write-ahead log: replay each buffered block to its target block); log `[HFS+] Journal replayed %u transactions`; this guarantees a consistent read-only image
- [ ] Log: `[HFS+] Mounted: blockSize=%u totalBlocks=%u free=%u variant=%s` where variant=`HFS+` or `HFSX`
- [ ] Commit: `"fs/hfsplus: volume header -- H+/HX magic, fork data, dirty flag, journal replay, block layout"`

## 10. HFS+ Catalog B-Tree + Directory Reader `[Opus]`

Walk the HFS+ Catalog B-tree. Search by `(parentCNID, filename)` key. Decode `HFSUniStr255` (UTF-16BE) filenames to UTF-8. List directories. Handle HFSX case-sensitivity.

**Files:** `src/kernel/fs/hfsplus/hfsplus_btree.c` (new), `src/kernel/fs/hfsplus/hfsplus_dir.c` (new)

> [!NOTE]
> B-tree node format: node descriptor at byte 0 of each node: `fLink (4)` (next node in leaf chain), `bLink (4)` (prev), `kind (1)` (0=leaf, 1=index, 2=header, -1=map), `height (1)`, `numRecords (2)`, `reserved (2)`. After descriptor: variable-length records with offsets stored at the *end* of the node as 2-byte big-endian values, counting from the last record backward. Header node (node 0): `BTHeaderRec` at record 0 -- `nodeSize (2)`, `maxKeyLength (2)`, `treeDepth (2)`, `rootNode (4)` (node number of root), `firstLeafNode (4)`. Catalog key: `HFSPlusCatalogKey { keyLength (2, big-endian), parentID (4, big-endian), nodeName HFSUniStr255 { length (2, big-endian), unicode[255] (2 bytes each, big-endian) } }`. Key comparison: first compare `parentID` (big-endian uint32), then `nodeName` -- for HFS+: case-insensitive using Apple's HFS+ Unicode folding table; for HFSX with `kHFSCaseFolded` attribute: same folding; for HFSX with `kHFSBinaryCompare`: byte-for-byte comparison. Catalog records: `HFSPlusCatalogFile` (`recordType = 2`) or `HFSPlusCatalogFolder` (`recordType = 1`).

- [ ] `hfsplus_btree_t` struct: `root_node`, `node_size`, `first_leaf`, `blkdev`, `fork` (Catalog or Extents); node LBA from fork extents
- [ ] `hfsplus_btree_open(vol, fork, &bt)`: read header node (node 0 of the fork's first extent); parse `BTHeaderRec`; store `root_node`, `node_size`
- [ ] `hfsplus_node_read(bt, node_num, &node_buf)`: map node number to physical block via fork extents; read `node_size` bytes
- [ ] `hfsplus_btree_search(bt, key_parentid, key_name_utf16, name_len, &record_out, &record_len)`: descend from root; at each index node: binary search records by `(parentID, nodeName)` comparison; at leaf: find matching record
- [ ] `hfsplus_str_to_utf8(unistr, out, out_len)`: decode `HFSUniStr255` (UTF-16BE big-endian) to UTF-8; handle surrogate pairs
- [ ] `hfsplus_readdir(vol, dir_cnid, callback, ctx)`: start from `firstLeafNode`; iterate leaf records; for each `HFSPlusCatalogKey` where `parentID == dir_cnid`: extract filename via `hfsplus_str_to_utf8()`; get child CNID from record value; call `callback`; advance to next record or next leaf node via `fLink`
- [ ] HFSX case-folding: if `vol->is_hfsx` and `kHFSCaseFolded` attribute set: apply Apple's HFS+ Unicode case-folding table (2-byte table, 33 KiB) during key comparison; otherwise binary compare
- [ ] VFS `finddir`: `hfsplus_vfs_finddir(vfs_node, name)` → `hfsplus_btree_search()` → build VFS node from catalog record
- [ ] VFS `readdir`: `hfsplus_vfs_readdir(vfs_node, index)` → `hfsplus_readdir()` counting to `index`-th entry
- [ ] Commit: `"fs/hfsplus: Catalog B-tree -- header node, key compare, leaf search, readdir, UTF-16BE decode"`

## 11. HFS+ File Read + Extents Overflow B-Tree `[Sonnet]`

Map CNID logical blocks to physical blocks using inline catalog extents. For fragmented files (> 8 extents), look up additional extents in the Extents Overflow B-tree.

**Files:** `src/kernel/fs/hfsplus/hfsplus_io.c` (new)

> [!NOTE]
> `HFSPlusForkData` in the catalog `HFSPlusCatalogFile` record: `logicalSize (8, big-endian)`, `clumpSize (4)`, `totalBlocks (4)`, `extents[8]` (each `HFSPlusExtentDescriptor { startBlock (4, BE), blockCount (4, BE) }`). The 8 inline extents cover the first up to `sum(extents[i].blockCount)` blocks. If `totalBlocks > sum(inline extent blocks)`: look up the Extents Overflow file (B-tree keyed by `(CNID, fork_type=0, start_block)`). Extents Overflow key: `HFSPlusExtentKey { keyLength (2), forkType (1), pad (1), fileID (4), startBlock (4) }`. Extents Overflow value: 8 more extent descriptors (same format as inline). Repeat until `totalBlocks` covered.

- [ ] `hfsplus_extent_read(vol, extents, n_extents, logical_block, &phys_block, &run_len)`: find the `extents[i]` whose cumulative block count covers `logical_block`; `phys_block = extents[i].startBlock + (logical_block - cumulative_start)`;` run_len = extents[i].blockCount - (logical_block - cumulative_start)`
- [ ] `hfsplus_overflow_lookup(vol, cnid, start_block, extra_extents_out)`: `hfsplus_btree_search()` on the Extents Overflow B-tree (separate `hfsplus_btree_t` opened from `vol->extents_fork`); key = `(0, cnid, start_block)`; return 8 more extent descriptors
- [ ] `hfsplus_file_read(vol, catalog_record, offset, buf, len)`: compute `logical_block = offset / block_size`; walk inline extents; if beyond inline extents: `hfsplus_overflow_lookup()` for additional extents; `blkdev_read()` for each physical block; loop until `len` bytes read
- [ ] VFS `read` callback: `hfsplus_vfs_read(vfs_node, buf, offset, len)` → `hfsplus_file_read()`
- [ ] Commit: `"fs/hfsplus: file read -- inline extents, Extents Overflow B-tree, CNID→physical block walk"`

## 12. VFS Registration -- `apfs_probe`, `hfsplus_probe`, Read-Only Mount `[Sonnet]`

Register both drivers with `vfs_probe()`. Probe APFS then HFS+ for Apple-partitioned volumes. Display "(read-only, Apple)" in volume properties for both.

**Files:** `src/kernel/fs/apfs/apfs_vfs.c` (new), `src/kernel/fs/hfsplus/hfsplus_vfs.c` (new)

> [!NOTE]
> `apfs_probe(blkdev)`: read block 0 (first `block_size` bytes -- but block size is not yet known; read 4096 bytes); check `o_type` field and magic bytes for `BSXN`; also check GPT partition type GUID via `GPT_GUID_APPLE_APFS` to avoid false positives on non-Apple devices. `hfsplus_probe(blkdev)`: read 512 bytes at offset 1024; check `signature` for `0x482B` or `0x4858`; also check GPT GUID `GPT_GUID_APPLE_HFS`. Both probes must call `gpt_guid_equal()` on the partition entry's GUID -- this prevents probing random NTFS or FAT volumes that happen to have `H+` bytes at offset 1024. Volume properties: `GetVolumeInformationW` for APFS returns volume name from `apfs_volname`, filesystem name `"APFS"`, serial from lower 32 bits of volume UUID; for HFS+ returns volume label from catalog root folder record, filesystem name `"HFS+"` or `"HFSX"`.

- [ ] `apfs_probe(blkdev_t *dev, gpt_part_t *part)`: if `part != NULL`: check `gpt_guid_equal(&part->type, &GPT_GUID_APPLE_APFS)`; read block 0; check `BSXN` magic + `o_type`; return 1 or 0
- [ ] `apfs_mount(blkdev_t *dev, char letter)` → full init: `apfs_container_parse()` → `apfs_enumerate_volumes()` → `vfs_mount(letter, &apfs_driver, root_vfs_node, VFS_READONLY)`
- [ ] APFS VFS driver vtable: read callbacks (`open`, `close`, `read`, `finddir`, `readdir`, `stat`, `readlink`, `xattr_get`) delegate to §5–§8; all write callbacks return `STATUS_MEDIA_WRITE_PROTECTED`
- [ ] `hfsplus_probe(blkdev_t *dev, gpt_part_t *part)`: if `part != NULL`: check `GPT_GUID_APPLE_HFS`; read offset 1024; check magic; return 1 or 0
- [ ] `hfsplus_mount(blkdev_t *dev, char letter)` → `hfsplus_vhdr_parse()` → `hfsplus_journal_replay()` → `hfsplus_btree_open()` → `vfs_mount(letter, &hfsplus_driver, root_vfs_node, VFS_READONLY)`
- [ ] HFS+ VFS driver vtable: read callbacks delegate to §9–§11; write callbacks return `STATUS_MEDIA_WRITE_PROTECTED`
- [ ] Volume properties: `"(read-only, Apple)"` string appended to volume label in File Manager sidebar; `GetVolumeInformationW` filesystem name = `"APFS"` / `"HFS+"` / `"HFSX"`
- [ ] Add `apfs_probe()` at step 8 and `hfsplus_probe()` at step 9 in `vfs_probe()` probe chain
- [ ] `apfs_umount(vol)`: free node cache; free volume list; free chunk map
- [ ] `hfsplus_umount(vol)`: free B-tree node cache; free volume struct
- [ ] Log: `[APFS] Mounted %c: "%s" (read-only)` / `[HFS+] Mounted %c: "%s" (read-only%s)` where `%s` = `, HFSX` for HFSX
- [ ] Commit: `"fs/apfs+hfsplus: VFS registration -- probe, read-only mount, GPT GUID guard, volume metadata"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐  | APFS container superblock + Fletcher-64 + checkpoint resolution | ❌ No native support (requires third-party | ❌ No upstream kernel driver; `apfs-fuse` | ⬜ §1 -- native in-kernel; Fletcher-64 verify; checkpoint |
| ⭐  | APFS object map B-tree                   | ❌ N/A                                   | ❌ `apfs-fuse` only (userspace FUSE)     | ⬜ §2 -- MVCC lookup (highest xid ≤      |
| ⭐  | APFS volume enumeration                  | ❌ N/A                                   | ❌ `apfs-fuse` only                      | ⬜ §3 -- `nx_fs_oid[100]` scan; sealed-volume detection + |
| ⭐  | APFS filesystem B-tree                   | ❌ N/A                                   | ❌ `apfs-fuse` only                      | ⬜ §4 -- virtual OID resolution via volume |
| ⭐  | APFS inodes                              | ❌ N/A                                   | ❌ `apfs-fuse` only                      | ⬜ §5 -- xf_blob extended field decode; nanosecond |
| ⭐  | APFS file extents + FileVault deny       | ❌ N/A                                   | ❌ `apfs-fuse` only; no FileVault handling | ⬜ §6 -- `FILE_EXTENT` walk; sparse zeros; `STATUS_ACCESS_DENIED` |
| ⭐  | APFS directory reader                    | ❌ N/A                                   | ❌ `apfs-fuse` only                      | ⬜ §7 -- `j_drec_hashed_key_t`; NFD→NFC for Windows display |
| ⭐  | APFS symlinks + xattrs -- inline + stream dnode | ❌ N/A                                   | ❌ `apfs-fuse` only                      | ⬜ §8 -- `com.apple.fs.symlink`; xattr stream via `xattr_obj_id` |
| ⭐  | HFS+ volume header + journal replay + fork data | ❌ No native support (requires `Paragon  | ✅ `hfsplus.ko`; full R/O + limited      | ⬜ §9 -- `H+`/`HX` magic; dirty-flag journal replay |
| ⭐  | HFS+ Catalog B-tree + readdir + `HFSUniStr255` UTF-16BE decode | ❌ No native support                     | ✅ `hfsplus.ko`; full Catalog B-tree; UTF-16BE | ⬜ §10 -- HFSX case-folding table; leaf chain |
| ⭐  | HFS+ file read + Extents Overflow B-tree for fragmented files | ❌ No native support                     | ✅ `hfsplus.ko`; Extents Overflow lookup; fragmented | ⬜ §11 -- inline extents + overflow B-tree |
| ⭐  | Dual read-only probe                     | ❌ No native support for either          | ✅ HFS+ via `hfsplus.ko`; APFS only      | ⬜ §12 -- both in-kernel; GPT GUID guard |

> **After §1–§12:** Impossible OS becomes the only non-Apple, non-Linux OS with native in-kernel APFS read support -- surpassing Windows 11 (which requires a paid Paragon driver for both formats). The in-kernel APFS driver with proper MVCC object map semantics, FileVault detection, and NFD→NFC filename normalization provides a cleaner experience than the `apfs-fuse` userspace approach on Linux. The GPT GUID guard on both probes prevents any false-positive mounts on non-Apple drives.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] APFS probe: APFS disk image in QEMU → `[APFS] Container: block_size=4096`; NTFS image → `apfs_probe` returns 0
- [ ] APFS Fletcher-64: corrupt 1 byte in APFS block 0 → `[APFS] checksum mismatch on block 0`; checkpoint scan finds backup
- [ ] APFS omap: `apfs_resolve_oid(fs_oid[0])` → valid physical block containing `APSB` magic
- [ ] APFS volume: `[APFS] Volume "Macintosh HD": files=N dirs=M` in serial log; sealed SNAP volume skipped
- [ ] APFS readdir: `FindFirstFileW("D:\\*.*")` on APFS volume enumerates root directory; filenames with accented characters display correctly (NFD→NFC)
- [ ] APFS file read: read a 1 MiB file from APFS; content byte-for-byte matches macOS-written original
- [ ] APFS FileVault: attempt `ReadFile` on encrypted APFS file → `ERROR_ACCESS_DENIED`; log `[APFS] FileVault encrypted file -- access denied`
- [ ] APFS symlink: `apfs_readlink()` on symlink inode returns correct UTF-8 target from `com.apple.fs.symlink` xattr
- [ ] HFS+ probe: HFS+ disk image → `[HFS+] Mounted D: "Untitled" (read-only)`; APFS image → `hfsplus_probe` returns 0
- [ ] HFS+ journal: dirty HFS+ volume (kHFSVolumeUnmountedBit=0) → `[HFS+] Journal replayed N transactions`; files readable after replay
- [ ] HFS+ readdir: `FindFirstFileW("E:\\*.*")` on HFS+ volume; UTF-16BE filenames with Japanese characters correct
- [ ] HFS+ overflow: file with > 8 extents; `hfsplus_file_read()` returns all bytes correctly (overflow B-tree used)
- [ ] HFSX: HFSX volume with `kHFSCaseFolded`; `finddir("FILE.TXT")` finds `file.txt` (case-insensitive)
- [ ] GPT guard: non-Apple NTFS partition where bytes at offset 1024 happen to be `0x482B`; `hfsplus_probe` returns 0 because GPT GUID does not match
- [ ] Write protection: `WriteFile` to APFS or HFS+ drive → `ERROR_WRITE_PROTECT`; no crash; no data written
- [ ] Commit: `"fs/apfs+hfsplus: complete read-only Apple filesystem drivers -- APFS omap/fstree/extents + HFS+ catalog/overflow"`
