# 040.13-APFS — Apple File System (Read-Only Driver)

> **Goal:** Implement a read-only APFS driver for Impossible OS. The driver must
> detect APFS containers via GPT, parse Container and Volume Superblocks, implement
> the Fletcher-64 checksum, scan the checkpoint descriptor ring for the latest valid
> state, resolve virtual OIDs through the Object Map B-tree, traverse the catalog
> B-tree for inode/directory/extent lookups, and read file data via file extent
> records. This enables reading files from macOS-formatted drives — essential for
> cross-platform data access and forensic recovery from Apple devices.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL B-tree node reads, OMAP
> cache buffers, extent data blocks, and directory data blocks. `kmalloc` is ONLY
> for small kernel structs (≤ 4 KB). See `coding.md` Known Gotchas.

> [!WARNING]
> **Read-Only First.** APFS write support requires implementing the full CoW
> checkpoint commit cycle — writing without it causes irrecoverable container
> corruption. This TODO covers **read-only** access only. Write support is a
> future P4 extension.

> [!IMPORTANT]
> **Byte Order:** All APFS on-disk structures are **little-endian**, EXCEPT the
> `nx_uuid` field in the container superblock which is **big-endian**. The driver
> must handle this single exception.
>
> **Spec Reference:** All offsets, field layouts, and algorithms reference the
> [APFS Specification](file:///home/derickpayne/impossible-os/specs/storage/filesystems/apfs.md).

---

## TODO Completion Roadmap (Cross-File)

> [!IMPORTANT]
> **Four TODO files and one spec** feed into the APFS driver. They have
> cross-dependencies that dictate implementation order.

### Dependency Graph

```mermaid
graph TD
    SPEC["specs/storage/filesystems/apfs.md<br/>APFS On-Disk Specification"]
    BLK["TODO-040.01-VirtIO / 040.02-AHCI<br/>Block Device Layer"]
    PART["TODO-040.05-GPT<br/>GPT Partition Detection"]
    VFS["TODO-040.07-VFS.md<br/>VFS Core + Win32 API"]

    A["§1.1 Container Superblock"]
    B["§1.2 Fletcher-64 Checksum"]
    C["§1.3 Checkpoint Discovery"]
    D["§2.1 Object Map"]
    E["§2.2 B-Tree Engine"]
    F["§3.1 Volume Superblock"]
    G["§3.2 Volume OMAP + Catalog"]
    H["§4.1 Inode Reader"]
    I["§4.2 Directory Records"]
    J["§4.3 Path Resolution"]
    K["§5.1 File Extent Reader"]
    L["§5.2 File Data Reader"]
    M["§6.1 VFS Registration"]
    N["§7.1 Test Suite"]
    O["§8.1 Space Manager"]
    P["§9.1 OMAP + Dentry Cache"]
    Q["§10.1 Container Health Dashboard"]
    R["§10.2 Clone Detective"]
    S["§10.3 Snapshot Explorer"]
    T["§10.4 Nanosecond Timestamp Inspector"]
    U["§11.1 LZVN/LZFSE Decompression"]
    V["§10.5 Volume Group Analyzer"]
    W["§10.6 Encryption Status Reporter"]
    X["§10.7 Fusion Drive Detector"]

    SPEC --> A
    BLK --> A
    PART --> A
    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
    G --> I
    H --> J
    I --> J
    H --> K
    K --> L
    L --> J
    J --> M
    VFS --> M
    M --> N
    D --> P
    H --> Q
    H --> R
    C --> S
    H --> T
    L --> U
    F --> ### Phase-by-Phase Implementation Order

| ⭐ | Phase | Sections                             | Depends On           | Status |
| -- | :---: | ------------------------------------ | -------------------- | :----: |
| 💎 | **0** | Prerequisites (spec, block, GPT)     | —                    |   ✅   |
| 💎 | **1** | §1.1 Container Superblock            | Phase 0              |   ⬜   |
| 💎 | **1** | §1.2 Fletcher-64 Checksum            | Phase 1 (§1.1)       |   ⬜   |
| 💎 | **1** | §1.3 Checkpoint Discovery            | Phase 1 (§1.2)       |   ⬜   |
| 💎 | **2** | §2.1 Object Map (OMAP)               | Phase 1 (§1.3)       |   ⬜   |
| 💎 | **2** | §2.2 B-Tree Engine                   | Phase 2 (§2.1)       |   ⬜   |
| 💎 | **3** | §3.1 Volume Superblock               | Phase 2 (§2.2)       |   ⬜   |
| 💎 | **3** | §3.2 Volume OMAP + Catalog           | Phase 3 (§3.1)       |   ⬜   |
| 💎 | **4** | §4.1 Inode Reader                    | Phase 3 (§3.2)       |   ⬜   |
| 💎 | **4** | §4.2 Directory Records               | Phase 3 (§3.2)       |   ⬜   |
| 💎 | **4** | §4.3 Path Resolution                 | Phase 4 (§4.1,§4.2)  |   ⬜   |
| 💎 | **5** | §5.1 File Extent Reader              | Phase 4 (§4.1)       |   ⬜   |
| 💎 | **5** | §5.2 File Data Reader                | Phase 5 (§5.1)       |   ⬜   |
| 💎 | **6** | §6.1 VFS Registration                | Phase 5 + VFS        |   ⬜   |
| 💎 | **7** | §7.1 Test Suite                      | Phase 6              |   ⬜   |
| 💎 | **7** | §8.1 Space Manager (read-only)       | Phase 1 (§1.3)       |   ⬜   |
| 💎 | **7** | §9.1 OMAP + Dentry Cache             | Phase 2 (§2.1)       |   ⬜   |
| 💎 | **7** | §11.1 LZVN/LZFSE Decompression       | Phase 5 (§5.2)       |   ⬜   |
| ⭐ | **8** | §10.1 Container Health Dashboard     | Phase 4 (§4.1)       |   ⬜   |
| ⭐ | **8** | §10.2 Clone Detective                | Phase 4 (§4.1)       |   ⬜   |
| ⭐ | **8** | §10.3 Snapshot Explorer              | Phase 1 (§1.3)       |   ⬜   |
| ⭐ | **8** | §10.4 Nanosecond Timestamp Inspector | Phase 4 (§4.1)       |   ⬜   |
| ⭐ | **8** | §10.5 Volume Group Analyzer          | Phase 3 (§3.1)       |   ⬜   |
| ⭐ | **8** | §10.6 Encryption Status Reporter     | Phase 3 (§3.1)       |   ⬜   |
| ⭐ | **8** | §10.7 Fusion Drive Detector          | Phase 1 (§1.1)       |   ⬜   |

> [!NOTE]
> **Phase 0** is already complete — block I/O and GPT partition tables work.
>
> **Phase 1** is the critical APFS bootstrap: parse block 0, implement the custom
> Fletcher-64 checksum, then scan the checkpoint ring to find the real active superblock.
>
> **Phases 2–3** build the two-level OID translation layer (container OMAP → volume OMAP)
> and the generic B-tree engine that all metadata lookups depend on.
>
> **Phases 4–5** read inodes, directories, extents, and file data — producing a mountable volume.
>
> **Phase 6** wires everything into VFS for drive-letter access.
>
> **Phase 7** adds robustness: caching, decompression, free space reporting, and testing.
>
> **Phase 8** delivers exclusive features (⭐): health dashboard, clone detective,
> snapshot explorer, nanosecond timestamps, volume group analysis, encryption status,
> and Fusion Drive detection.g, decompression, free space reporting, and testing.
>
> **Phase 8** delivers exclusive features (⭐): health dashboard, clone detective,
> snapshot explorer, and nanosecond timestamp inspector.

> [!TIP]
> **Critical gotcha — block 0 superblock is stale:** The block 0 CSB is only a
> bootstrap copy. The driver **must** scan the checkpoint descriptor area to find
> the superblock with the highest valid `o_xid`. Using block 0 directly for state
> tracking will cause data loss.
>
> **Critical gotcha — OMAP caching is mandatory:** Every virtual object read
> requires an OMAP B-tree lookup. Without caching OMAP branch nodes, the driver
> incurs O(log N) disk reads per file access — catastrophic on large volumes.
>
> **Memory rule reminder:** B-tree nodes are full blocks (4096 bytes). Always use
> `pmm_alloc_contiguous()` for node reads. The OMAP cache should pin upper-level
> branch nodes permanently.
>
> **QEMU testing:** APFS images must be pre-formatted on macOS or via `apfsprogs`.
> ```
> qemu-system-x86_64 -m 512M -bios /usr/share/ovmf/OVMF.fd \
>     -drive file=system-disk.img,format=raw,if=none,id=boot \
>     -device virtio-blk-pci,drive=boot \
>     -drive file=apfs-test.img,format=raw,if=none,id=apfs \
>     -device virtio-blk-pci,drive=apfs
> ```

---
## 1. Container Bootstrap

### 1.1 Container Superblock Parser

**Prompt:** Read the Container Superblock (CSB) from block 0 of the APFS partition. Validate the object header checksum (Fletcher-64 — implemented in §1.2). Verify `nx_magic` at offset `0x20` equals `0x4253584E` (`"NXSB"`). Extract critical fields: `nx_block_size` (offset `0x24`), `nx_block_count` (`0x28`), `nx_uuid` (`0x48`, big-endian), checkpoint descriptor area base/size (`0x70`/`0x68`), `nx_omap_oid` (`0xA0`), `nx_max_file_systems` (`0xB4`), and `nx_fs_oid[]` volume OID array (`0xB8`). Note: block 0 CSB is only a bootstrap — §1.3 finds the real active CSB. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: container superblock parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Read block 0 (4096 bytes) from partition start
- [ ] Parse `obj_phys_t` header (32 bytes): `o_cksum`, `o_oid`, `o_xid`, `o_type`, `o_subtype`
- [ ] Validate `o_type & 0xFFFF == 0x0001` (`OBJECT_TYPE_NX_SUPERBLOCK`)
- [ ] Validate `nx_magic` at `0x20` == `0x4253584E`
- [ ] Extract container geometry:
  - [ ] `nx_block_size` at `0x24` (4B) — typically 4096
  - [ ] `nx_block_count` at `0x28` (8B) — total blocks in container
- [ ] Extract feature flags:
  - [ ] `nx_features` at `0x30` (8B) — compatible features
  - [ ] `nx_readonly_compatible_features` at `0x38` (8B)
  - [ ] `nx_incompatible_features` at `0x40` (8B)
- [ ] Extract `nx_uuid` at `0x48` (16B) — **big-endian** (only exception)
- [ ] Extract checkpoint area layout:
  - [ ] `nx_xp_desc_blocks` at `0x68` (4B) — descriptor ring size
  - [ ] `nx_xp_data_blocks` at `0x6C` (4B) — data area size
  - [ ] `nx_xp_desc_base` at `0x70` (8B) — descriptor ring base block
  - [ ] `nx_xp_data_base` at `0x78` (8B) — data area base block
  - [ ] `nx_xp_desc_index` at `0x88` (4B) — current descriptor index
  - [ ] `nx_xp_desc_len` at `0x8C` (4B) — active descriptor count
- [ ] Extract key OIDs:
  - [ ] `nx_spaceman_oid` at `0x98` (8B) — Space Manager virtual OID
  - [ ] `nx_omap_oid` at `0xA0` (8B) — Container OMAP physical address
  - [ ] `nx_reaper_oid` at `0xA8` (8B) — Reaper virtual OID
- [ ] Extract volume list:
  - [ ] `nx_max_file_systems` at `0xB4` (4B) — max volumes (default 100)
  - [ ] `nx_fs_oid[100]` at `0xB8` — volume superblock virtual OIDs
- [ ] Log: `[apfs] Container: %llu blocks, block_size=%u, UUID=%s`
- [ ] Log: `[apfs] Checkpoint area: desc_base=%llu, desc_blocks=%u`
- [ ] Commit: `"apfs: container superblock parser"`

### 1.2 Fletcher-64 Checksum

**Prompt:** Implement the APFS-specific Fletcher-64 checksum. This is NOT standard RFC 1146 Fletcher — the modulus is `0xFFFFFFFF` and the final step computes complementary values. The checksum covers bytes 8 through block_size-1 (excluding the 8-byte `o_cksum` field). Process data as 32-bit words. Every metadata block read must be validated. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: fletcher-64 checksum"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `apfs_fletcher64(data, length)`:
  - [ ] Initialize `sum1 = 0`, `sum2 = 0` (both 64-bit to avoid overflow)
  - [ ] Process bytes 8 through end as array of 32-bit little-endian words
  - [ ] For each word: `sum1 = (sum1 + word) % 0xFFFFFFFF`, `sum2 = (sum2 + sum1) % 0xFFFFFFFF`
  - [ ] Compute `check1 = 0xFFFFFFFF - ((sum1 + sum2) % 0xFFFFFFFF)`
  - [ ] Compute `check2 = 0xFFFFFFFF - ((sum1 + check1) % 0xFFFFFFFF)`
  - [ ] Return `(check2 << 32) | check1` as uint64_t
- [ ] Implement `apfs_verify_block(block_data, block_size)`:
  - [ ] Compute Fletcher-64 over bytes 8..block_size-1
  - [ ] Compare against `o_cksum` at offset 0
  - [ ] Return true/false
- [ ] Validate block 0 CSB checksum (retroactively verify §1.1)
- [ ] Log on failure: `[apfs] CHECKSUM FAILED: block %llu`
- [ ] Commit: `"apfs: fletcher-64 checksum"`

### 1.3 Checkpoint Discovery

**Prompt:** Scan the checkpoint descriptor ring buffer to find the active container superblock. The descriptor area is a circular buffer at `nx_xp_desc_base` with `nx_xp_desc_blocks` entries. Scan each block, validate Fletcher-64, check for type `OBJECT_TYPE_NX_SUPERBLOCK`, and track the highest valid `o_xid`. Then locate the Checkpoint Map entries to find all ephemeral objects. Per APFS spec §Checkpoint System. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: checkpoint discovery"`. After implementation, save any gotchas to MCP memory.

- [ ] Read blocks from `nx_xp_desc_base` through `nx_xp_desc_base + nx_xp_desc_blocks - 1`
- [ ] For each block in the descriptor ring:
  - [ ] Validate Fletcher-64 checksum — skip if invalid
  - [ ] Check `o_type & 0xFFFF` for `OBJECT_TYPE_NX_SUPERBLOCK` (`0x0001`)
  - [ ] Track block with highest valid `o_xid`
- [ ] Use highest-`o_xid` block as **active container superblock** (replace block 0 copy)
- [ ] Re-extract all CSB fields from the active superblock
- [ ] Locate Checkpoint Map:
  - [ ] Find blocks with `o_type & 0xFFFF == 0x000C` (`OBJECT_TYPE_CHECKPOINT_MAP`)
  - [ ] Parse `checkpoint_mapping_t` entries (40 bytes each):
    - [ ] `cpm_type` (4B), `cpm_subtype` (4B), `cpm_size` (4B), `cpm_pad` (4B)
    - [ ] `cpm_fs_oid` (8B), `cpm_oid` (8B), `cpm_paddr` (8B)
  - [ ] Stop when `CHECKPOINT_MAP_LAST` flag (`0x00000001`) is set
- [ ] Build ephemeral object table: map virtual OID → physical address
- [ ] Log: `[apfs] Active checkpoint: xid=%llu (scanned %u descriptors)`
- [ ] Commit: `"apfs: checkpoint discovery"`

---

## 2. Object Map & B-Tree Engine

### 2.1 Object Map (OMAP)

**Prompt:** Parse the container Object Map from `nx_omap_oid` (physical address). The OMAP is a B-tree that maps virtual OIDs to physical block addresses. Parse the `omap_phys_t` header to get the B-tree root OID (`om_tree_oid`). Implement lookup: given a virtual OID and transaction ID, search the OMAP B-tree for the key `(oid, xid)` where `xid ≤ current_xid`, return the physical address. Per APFS spec §Object Map. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: object map"`. After implementation, save any gotchas to MCP memory.

- [ ] Read OMAP block at `nx_omap_oid` (physical address — no OMAP lookup needed)
- [ ] Validate Fletcher-64 checksum
- [ ] Parse `omap_phys_t`:
  - [ ] `om_flags` (4B), `om_snap_count` (4B)
  - [ ] `om_tree_type` (4B), `om_snapshot_tree_type` (4B)
  - [ ] `om_tree_oid` (8B) — physical OID of the mapping B-tree root
  - [ ] `om_snapshot_tree_oid` (8B)
  - [ ] `om_most_recent_snap` (8B)
- [ ] Implement `apfs_omap_lookup(omap, virtual_oid, xid)`:
  - [ ] Search B-tree for key `(oid, xid)` where `xid ≤ current_xid`
  - [ ] Return `ov_paddr` (physical block address)
  - [ ] Handle not-found → error
- [ ] Parse OMAP B-tree key: `omap_key_t` — `ok_oid` (8B) + `ok_xid` (8B)
- [ ] Parse OMAP B-tree value: `omap_val_t` — `ov_flags` (4B) + `ov_size` (4B) + `ov_paddr` (8B)
- [ ] Log: `[apfs] OMAP: tree at block %llu, %u snapshots`
- [ ] Commit: `"apfs: object map"`

### 2.2 Generic B-Tree Engine

**Prompt:** Implement a generic B-tree traversal engine used by all APFS B-trees (OMAP, catalog, extent ref). Parse `btree_node_phys_t` headers: flags, level, key count, TOC layout. Handle both fixed-size (`BTNODE_FIXED_KV_SIZE`) and variable-size TOC entries. Navigate from root through internal nodes to leaf nodes using binary search. The bidirectional memory layout has keys growing forward and values growing backward. Per APFS spec §B-Tree Architecture. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: generic b-tree engine"`. After implementation, save any gotchas to MCP memory.

- [ ] Parse `btree_node_phys_t` header:
  - [ ] `btn_flags` (2B): `BTNODE_ROOT` (0x0001), `BTNODE_LEAF` (0x0002), `BTNODE_FIXED_KV_SIZE` (0x0004)
  - [ ] `btn_level` (2B): 0=leaf, >0=branch
  - [ ] `btn_nkeys` (4B): number of keys in this node
  - [ ] `btn_table_space` (4B): TOC offset and length
  - [ ] `btn_free_space`, `btn_key_free_list`, `btn_val_free_list`
- [ ] Parse Table of Contents entries:
  - [ ] Fixed-size: 4 bytes — 2B key_offset + 2B val_offset
  - [ ] Variable-size: 6 bytes — 2B key_off + 2B key_len + 2B val_off
- [ ] Navigate bidirectional layout:
  - [ ] Keys start after TOC, grow forward
  - [ ] Values start at block end, grow backward
  - [ ] Key data at: `node_base + sizeof(header) + toc_size + key_offset`
  - [ ] Value data at: `block_end - val_offset` (for variable) or calculated from end
- [ ] If `BTNODE_ROOT`: parse `btree_info_t` at block end:
  - [ ] `bt_longest_key` (4B), `bt_longest_val` (4B)
  - [ ] `bt_key_count` (8B), `bt_node_count` (8B)
- [ ] Implement `apfs_btree_search(root_block, key, compare_fn, result)`:
  - [ ] At branch nodes: binary search keys, follow child pointer to next level
  - [ ] At leaf nodes: binary search keys, return matching value
  - [ ] Child pointers in branch values are physical block numbers (for physical trees) or virtual OIDs (for virtual trees requiring OMAP lookup)
- [ ] Implement `apfs_btree_iterate(root_block, callback)` — enumerate all leaf entries
- [ ] Log: `[apfs] B-tree node: level=%u, keys=%u, flags=0x%04X`
- [ ] Commit: `"apfs: generic b-tree engine"`

---

## 3. Volume Mounting

### 3.1 Volume Superblock Parser

**Prompt:** Resolve volume superblock OIDs from `nx_fs_oid[]` through the container OMAP. Validate `apfs_magic` at offset within the volume superblock equals `0x42535041` (`"APSB"`). Extract: volume feature flags, volume OMAP OID, catalog B-tree root OID, extent reference tree OID, snapshot metadata tree OID. Parse volume flags (`APFS_FS_UNENCRYPTED`, etc.) and incompatible features (`APFS_INCOMPAT_CASE_INSENSITIVE`, etc.). Per APFS spec §Volume Superblock. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: volume superblock parser"`. After implementation, save any gotchas to MCP memory.

- [ ] For each non-zero entry in `nx_fs_oid[]`:
  - [ ] Resolve virtual OID via container OMAP → physical block
  - [ ] Read block, validate Fletcher-64
  - [ ] Verify `apfs_magic` == `0x42535041`
- [ ] Parse volume superblock fields:
  - [ ] `apfs_fs_index` (4B) — index in container volume array
  - [ ] `apfs_features` (8B), `apfs_readonly_compatible_features` (8B)
  - [ ] `apfs_incompatible_features` (8B)
  - [ ] `apfs_unmount_time` (8B) — last unmount timestamp (nanoseconds)
  - [ ] `apfs_fs_reserve_block_count` (8B) — reserved blocks
  - [ ] `apfs_fs_quota_block_count` (8B) — max blocks
  - [ ] `apfs_fs_alloc_count` (8B) — currently allocated
- [ ] Extract critical OIDs:
  - [ ] `apfs_omap_oid` — volume Object Map physical OID
  - [ ] `apfs_root_tree_oid` — catalog B-tree root virtual OID
  - [ ] `apfs_extentref_tree_oid` — extent reference tree virtual OID
  - [ ] `apfs_snap_meta_tree_oid` — snapshot metadata tree virtual OID
- [ ] Parse volume flags:
  - [ ] `APFS_FS_UNENCRYPTED` (`0x01`) — no encryption
  - [ ] `APFS_FS_ONEKEY` (`0x08`) — single VEK for volume
  - [ ] Reject encrypted volumes unless `APFS_FS_UNENCRYPTED` is set
- [ ] Parse incompatible features:
  - [ ] `APFS_INCOMPAT_CASE_INSENSITIVE` (`0x01`) — case-insensitive names
  - [ ] `APFS_INCOMPAT_NORMALIZATION_INSENSITIVE` (`0x08`) — Unicode normalization
- [ ] Log: `[apfs] Volume %u: %llu blocks allocated, quota=%llu, flags=0x%X`
- [ ] Commit: `"apfs: volume superblock parser"`

### 3.2 Volume OMAP & Catalog Setup

**Prompt:** Load the volume's own Object Map from `apfs_omap_oid` and resolve the catalog B-tree root via the volume OMAP. The catalog B-tree (subtype `OBJECT_TYPE_FSTREE`, `0x000E`) is the central metadata structure holding all inodes, directory records, and file extents. Set up the catalog for lookup. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: volume omap and catalog setup"`. After implementation, save any gotchas to MCP memory.

- [ ] Read volume OMAP from `apfs_omap_oid` (physical address)
- [ ] Parse `omap_phys_t` — get volume OMAP B-tree root
- [ ] Resolve `apfs_root_tree_oid` (virtual) via volume OMAP → catalog root block
- [ ] Validate catalog root: `o_subtype` should be `0x000E` (`OBJECT_TYPE_FSTREE`)
- [ ] Store catalog root reference in `struct apfs_volume`
- [ ] Log: `[apfs] Catalog B-tree at block %llu (via volume OMAP)`
- [ ] Commit: `"apfs: volume omap and catalog setup"`

---

## 4. Inode & Directory Reading

### 4.1 Inode Reader

**Prompt:** Read inode records from the catalog B-tree. Catalog keys encode the Object ID and record type in a single 64-bit field (`obj_id_and_type`): bits 63:60 = type, bits 59:0 = OID. For inodes, type = `0x3` (`APFS_TYPE_INODE`). Parse `j_inode_val_t`: parent_id, timestamps (nanosecond precision), mode, owner, group, nlink/nchildren, internal flags. Handle extended fields (`xf_blob_t`). The root directory inode is OID 2. Per APFS spec §Inode Records. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: inode reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `apfs_read_inode(vol, oid, inode_out)`:
  - [ ] Build catalog key: `(oid & 0x0FFFFFFFFFFFFFFF) | (0x3ULL << 60)`
  - [ ] Search catalog B-tree for this key
  - [ ] Parse `j_inode_val_t` (variable size):
    - [ ] `parent_id` (8B), `private_id` (8B)
    - [ ] `create_time` (8B), `mod_time` (8B), `change_time` (8B), `access_time` (8B) — ns since epoch
    - [ ] `internal_flags` (8B)
    - [ ] `nchildren` / `nlink` (4B union)
    - [ ] `default_protection_class` (4B), `write_generation_counter` (4B)
    - [ ] `bsd_flags` (4B), `owner` (4B), `group` (4B)
    - [ ] `mode` (2B) — POSIX file type + permissions
    - [ ] `uncompressed_size` (8B) — if `INODE_HAS_UNCOMPRESSED_SIZE`
- [ ] Parse file type from `mode & S_IFMT`:
  - [ ] `0040000` = directory, `0100000` = regular file, `0120000` = symlink
- [ ] Parse internal flags: `INODE_IS_SPARSE`, `INODE_WAS_CLONED`, `INODE_HAS_FINDER_INFO`, etc.
- [ ] Parse extended fields (`xf_blob_t`) if data extends past base struct:
  - [ ] `xf_num_exts` (2B), `xf_used_data` (2B)
  - [ ] Walk `x_field_t` entries: `x_type` (1B), `x_flags` (1B), `x_size` (2B) + data
- [ ] Read root directory: inode OID 2
- [ ] Log: `[apfs] Inode %llu: mode=0x%04X, size=%llu, links=%u`
- [ ] Commit: `"apfs: inode reader"`

### 4.2 Directory Record Reader

**Prompt:** Read directory entries from the catalog B-tree. Directory records have type `0x9` (`APFS_TYPE_DIR_REC`) with the parent directory's OID. Keys include the UTF-8 filename. Values include the target inode OID and timestamp. Handle both leaf format (`j_drec_key_t` with `name_len` + `name[]`) and branch format (`j_drec_hashed_key_t` with hash). Enumerate all entries for a directory by iterating catalog records with matching parent OID. Per APFS spec §Directory Records. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: directory record reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Parse directory record key (`j_drec_key_t`):
  - [ ] `obj_id_and_type`: parent OID (bits 59:0) + type 0x9 (bits 63:60)
  - [ ] `name_len` (2B) — includes NUL terminator
  - [ ] `name[]` — variable-length UTF-8
- [ ] Parse hashed key variant (`j_drec_hashed_key_t`):
  - [ ] `name_len_and_hash` (4B): bits 31:22 = hash, bits 21:0 = name length
- [ ] Parse directory record value (`j_drec_val_t`):
  - [ ] `file_id` (8B) — target inode OID
  - [ ] `date_added` (8B) — ns timestamp
  - [ ] `flags` (2B)
- [ ] Implement `apfs_readdir(vol, dir_oid, callback)`:
  - [ ] Build range key: `(dir_oid & OBJ_ID_MASK) | (0x9ULL << 60)`
  - [ ] Iterate catalog B-tree for all entries with this parent OID and type
  - [ ] For each entry: invoke callback with name, target OID, flags
- [ ] Implement `apfs_finddir(vol, dir_oid, name)`:
  - [ ] Search catalog for specific directory record matching parent OID + name
  - [ ] Handle case-insensitive comparison if `APFS_INCOMPAT_CASE_INSENSITIVE` set
  - [ ] Return target inode OID
- [ ] Commit: `"apfs: directory record reader"`

### 4.3 Path Resolution

**Prompt:** Resolve full paths by splitting on `/`, starting at inode OID 2 (root directory), and recursively looking up each component via `apfs_finddir()`. Handle symlinks by reading the symlink target from the inode's data stream. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: path resolution"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `apfs_resolve_path(vol, path)`:
  - [ ] Start at root directory OID 2
  - [ ] Split path by `/`
  - [ ] For each component: call `apfs_finddir(vol, current_oid, name)`
  - [ ] If found → update current_oid, continue
  - [ ] If not found → return error
- [ ] Handle symlinks (`mode & S_IFMT == S_IFLNK`):
  - [ ] Read symlink target from data stream
  - [ ] Recursion limit: max 8 follows
- [ ] Handle case-insensitive volumes
- [ ] Commit: `"apfs: path resolution"`

---

## 5. File Data Reading

### 5.1 File Extent Reader

**Prompt:** Read file extent records from the catalog B-tree. Extent records have type `0x8` (`APFS_TYPE_FILE_EXTENT`). Keys include the logical byte offset. Values include physical block number, length (in bytes), and crypto ID. Handle multiple extents per file by iterating all extent records for the file's `private_id`. Per APFS spec §File Extents. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: file extent reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Parse file extent key (`j_file_extent_key_t`):
  - [ ] `obj_id_and_type`: private_id + type 0x8
  - [ ] `logical_addr` (8B) — logical byte offset in file
- [ ] Parse file extent value (`j_file_extent_val_t`):
  - [ ] `len_and_flags` (8B): bits 55:0 = length in bytes, bits 63:56 = flags
  - [ ] `phys_block_num` (8B) — starting physical block
  - [ ] `crypto_id` (8B) — encryption tweak (0 if unencrypted)
- [ ] Extract length: `len_and_flags & 0x00FFFFFFFFFFFFFF`
- [ ] Implement `apfs_get_extents(vol, private_id, extent_list)`:
  - [ ] Build range key with private_id + type 0x8
  - [ ] Iterate catalog for all matching extent records
  - [ ] Sort by `logical_addr`
  - [ ] Build ordered extent list
- [ ] Validate: extent length must be multiple of `nx_block_size`
- [ ] Log: `[apfs] Extent: logical=%llu → physical=%llu, len=%llu`
- [ ] Commit: `"apfs: file extent reader"`

### 5.2 File Data Reader

**Prompt:** Using the extent list, implement reading file data by byte range. Convert offset/length to logical block ranges, look up extents, translate to physical blocks, read from disk. Handle reads spanning multiple extents. Handle sparse files (gaps between extents → return zeros). Cap reads at file size from inode. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: file data reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `apfs_read_data(vol, inode, offset, length, buffer)`:
  - [ ] Get extent list for inode's `private_id`
  - [ ] For the requested byte range:
    - [ ] Find extent covering current offset
    - [ ] Calculate physical block: `phys_block_num + (offset - logical_addr) / block_size`
    - [ ] Read blocks from disk
  - [ ] Handle gaps between extents → fill with zeros (sparse)
  - [ ] Handle reads spanning multiple extents
  - [ ] Cap at inode file size
- [ ] Handle partial block reads at start/end of range
- [ ] Commit: `"apfs: file data reader"`

---

## 6. VFS Integration

### 6.1 APFS VFS Driver Registration

**Prompt:** Register APFS as a VFS filesystem driver. Implement `vfs_ops` callbacks: `open`, `close`, `read`, `readdir`, `finddir`, `stat`. Write callbacks return `-EROFS`. Detect APFS partitions via GPT type GUID `7C3457EF-0000-11AA-AA11-00306543ECAC` during partition scanning. On mount: run the full container bootstrap (CSB → checkpoint → OMAP → volume), then create VFS nodes for each unencrypted volume. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: VFS driver registration"`. After implementation, save any gotchas to MCP memory.

- [ ] Create `src/kernel/fs/apfs.c` and `include/kernel/fs/apfs.h`
- [ ] Define `struct apfs_container` — CSB data, OMAP, volumes
- [ ] Define `struct apfs_volume` — volume superblock, catalog root, OMAP
- [ ] Implement `apfs_detect(blkdev)`:
  - [ ] Read block 0, check magic `0x4253584E` at offset `0x20`
- [ ] Register with partition scanner:
  - [ ] GPT GUID `7C3457EF-0000-11AA-AA11-00306543ECAC`
- [ ] Implement `apfs_mount(blkdev)`:
  - [ ] Run full bootstrap: CSB → Fletcher-64 → checkpoint → OMAP → volumes
  - [ ] For each unencrypted volume: create VFS mount
- [ ] Implement `vfs_ops` callbacks:
  - [ ] `apfs_open(node, flags)` — resolve inode, cache extents
  - [ ] `apfs_close(node)` — free cached data
  - [ ] `apfs_read(node, offset, size, buf)` — extent-based read
  - [ ] `apfs_readdir(node, index)` — catalog enumeration
  - [ ] `apfs_finddir(node, name)` — catalog lookup
  - [ ] `apfs_stat(node, stat)` — populate from inode (nanosecond timestamps)
  - [ ] Write ops → return `-EROFS`
- [ ] Log: `[apfs] Mounted volume '%s' on drive %c: (%llu bytes)`
- [ ] Commit: `"apfs: VFS driver registration"`

---

## 7. Testing & Validation

### 7.1 APFS Test Suite

**Prompt:** Create APFS test disk images using macOS `hdiutil` or Linux `apfsprogs`. Test: container detection, checkpoint scanning, volume mounting, file reading, directory listing, path resolution, and Fletcher-64 validation. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"test: APFS filesystem test suite"`. After implementation, save any gotchas to MCP memory.

- [ ] Test image: APFS container with single volume, files in root
  - [ ] Verify: container parsing, checkpoint scan, volume mount, dir listing
- [ ] Test image: file with known content → read and compare byte-exact
- [ ] Test image: large file (>4 MB, multiple extents)
- [ ] Test image: deep directory path (`a/b/c/d/e/file.txt`)
- [ ] Test image: case-insensitive volume (verify lookup works)
- [ ] Test image: multi-volume container (2+ volumes sharing space)
- [ ] Test: Fletcher-64 with deliberately corrupted block → verify rejection
- [ ] QEMU: `-drive file=apfs-test.img,format=raw,if=none,id=apfs -device virtio-blk-pci,drive=apfs`
- [ ] Commit: `"test: APFS filesystem test suite"`

---

## 8. Space Manager (Read-Only)

### 8.1 Space Manager

**Prompt:** Parse the Space Manager (`spaceman_phys_t`) for free space reporting. Resolve via `nx_spaceman_oid`. Read the bitmap hierarchy (CABs → CIBs → bitmap blocks). Compute total free blocks for Disk Manager display. Read-only — no allocations. Per APFS spec §Space Manager. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: space manager read-only"`. After implementation, save any gotchas to MCP memory.

- [ ] Resolve `nx_spaceman_oid` via container OMAP (it's a virtual OID)
- [ ] Parse `spaceman_phys_t`: block_size, blocks_per_chunk, chunks_per_cib, cibs_per_cab
- [ ] Navigate bitmap hierarchy: CABs (0x0006) → CIBs (0x0007) → bitmap blocks
- [ ] Count free blocks (bit=0) across all bitmaps
- [ ] Report: total blocks, used blocks, free blocks, free percentage
- [ ] Wire to Disk Manager volume properties
- [ ] Log: `[apfs] Space: %llu/%llu blocks free (%.1f%%)`
- [ ] Commit: `"apfs: space manager read-only"`

---

## 9. Performance Optimization

### 9.1 OMAP & Dentry Cache

**Prompt:** Cache OMAP B-tree branch nodes in RAM to avoid O(log N) disk reads per virtual OID lookup. Implement an LRU dentry cache for path-to-inode mappings. Pin root directory inode and upper OMAP levels. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: omap and dentry cache"`. After implementation, save any gotchas to MCP memory.

- [ ] Pin OMAP root + top 2 branch levels in RAM at mount time
- [ ] Implement LRU cache for OMAP leaf lookups:
  - [ ] Key: virtual OID, Value: physical block address
  - [ ] Default 256 entries (configurable via Registry: `HKLM\SYSTEM\Storage\apfs\OmapCacheSize`)
- [ ] Implement dentry cache:
  - [ ] Key: (parent_oid, name_hash), Value: child inode OID
  - [ ] Default 128 entries
  - [ ] Pin root directory entries
- [ ] Telemetry: log on unmount: `[apfs] OMAP cache: %u hits / %u lookups (%.1f%%)`
- [ ] Commit: `"apfs: omap and dentry cache"`

---

## 10. APFS Volume Intelligence (🚀 Impossible OS Exclusive)

### 10.1 Container Health Dashboard

**Prompt:** Aggregate APFS container health into a Disk Manager panel. Read: checkpoint validity (are all descriptors checksumming correctly?), volume count vs max_volumes, space allocation per volume, OMAP snapshot count, reaper status, last unmount timestamp. Display with health score (✅/⚠️/❌). Neither macOS, Windows, nor Linux surfaces APFS container-level health in a GUI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: container health dashboard"`. After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** macOS `diskutil apfs list` shows basic volume info via CLI.
> Windows has zero native APFS support. Linux `apfs-fuse` is read-only with no health
> reporting. Impossible OS can be the first to show APFS container health — checkpoint
> integrity, per-volume space, OMAP stats — in a graphical dashboard.

- [ ] Read checkpoint ring: count valid vs invalid descriptors
  - [ ] All valid ✅, some invalid ⚠️ (ring corruption)
- [ ] Volume space allocation: per-volume `alloc_count` vs `quota` vs container total
- [ ] Snapshot count from OMAP header
- [ ] Last unmount time: `apfs_unmount_time` → human-readable timestamp
- [ ] Reaper status: check if Reaper has pending deletions
- [ ] Aggregate health score
- [ ] Wire to Disk Manager: APFS container properties panel
- [ ] Commit: `"apfs: container health dashboard"`

### 10.2 Clone Detective (🚀 Impossible OS Exclusive)

**Prompt:** APFS's zero-cost cloning creates shared extents tracked by physical extent reference counts (`refcnt` in `j_phys_ext_val_t`). Implement a clone detective that scans the extent reference tree to identify files sharing physical blocks. Show: which files share extents, how much space is saved by cloning, and a clone family tree. No OS surfaces clone relationships in a GUI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: clone detective"`. After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** macOS doesn't show which files are clones of each other.
> Windows and Linux have no APFS support. Impossible OS showing clone relationships
> and shared-block savings is a unique forensic feature.

- [ ] Scan extent reference tree (`OBJECT_TYPE_BLOCKREFTREE`, subtype `0x000F`)
- [ ] Identify physical extents with `refcnt > 1` — these are shared (cloned)
- [ ] For each shared extent: trace back to owning inode OIDs
- [ ] Build clone family map: group files sharing blocks
- [ ] Calculate savings: `shared_blocks × (refcnt - 1) × block_size`
- [ ] Wire to Disk Manager: "Clone Analysis" tab
- [ ] Commit: `"apfs: clone detective"`

### 10.3 Snapshot Explorer (🚀 Impossible OS Exclusive)

**Prompt:** APFS supports point-in-time volume snapshots. Parse the snapshot metadata tree (`OBJECT_TYPE_SNAPMETATREE`, subtype `0x0010`) to list snapshots with their names, creation times, and transaction IDs. Allow browsing a snapshot's filesystem state by using the snapshot's XID for OMAP lookups. No OS other than macOS CLI tools can browse APFS snapshots. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: snapshot explorer"`. After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** macOS `tmutil listlocalsnapshots` is CLI-only. Windows and
> Linux have no snapshot browsing. Impossible OS with a GUI snapshot browser with
> file-level diff would be a unique feature.

- [ ] Parse snapshot metadata tree from `apfs_snap_meta_tree_oid`
- [ ] Parse `APFS_TYPE_SNAP_METADATA` (0x1) and `APFS_TYPE_SNAP_NAME` (0xB) records
- [ ] List snapshots: name, creation XID, creation time
- [ ] Browse snapshot state: use snapshot XID for OMAP lookups
  - [ ] All virtual OID resolutions use `xid ≤ snapshot_xid`
- [ ] Wire to Disk Manager: "Snapshots" tab with list + browse
- [ ] Commit: `"apfs: snapshot explorer"`

### 10.4 Nanosecond Timestamp Inspector (🚀 Impossible OS Exclusive)

**Prompt:** APFS stores all timestamps with nanosecond precision (uint64_t, nanoseconds since Unix epoch). Display all 4 timestamps (create, modify, change, access) in file properties with full nanosecond resolution. macOS Finder shows second-resolution timestamps. Windows has no APFS support. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: nanosecond timestamp inspector"`. After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** macOS Finder shows timestamps rounded to seconds. `stat` shows
> nanoseconds but only via CLI. Impossible OS showing full ns precision in a GUI file
> properties panel is a unique capability.

- [ ] Parse APFS nanosecond timestamps: `value / 1000000000` = seconds, `value % 1000000000` = ns
- [ ] Display in file properties:
  - [ ] Created: `create_time` — always available (unlike ext2/ext3)
  - [ ] Modified: `mod_time`
  - [ ] Changed: `change_time` — metadata change
  - [ ] Accessed: `access_time`
- [ ] Format: `YYYY-MM-DD HH:MM:SS.nnnnnnnnn` (9 decimal places)
- [ ] Wire to File Properties panel for APFS volumes
- [ ] Commit: `"apfs: nanosecond timestamp inspector"`

---

## 11. Transparent Decompression

### 11.1 LZVN/LZFSE Decompression

**Prompt:** APFS uses the `decmpfs` framework for transparent compression. Compressed files have an extended attribute `com.apple.decmpfs` containing a header with compression type and the data (inline or in resource fork). Implement LZVN and LZFSE decoders. Without these, reading compressed files returns raw compressed data — causing silent data corruption. LZVN blocks exceeding 65,536 bytes indicate corruption. Per APFS spec §Transparent Compression. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apfs: lzvn lzfse decompression"`. After implementation, save any gotchas to MCP memory.

- [ ] Detect compressed files: check for `com.apple.decmpfs` xattr on inode
- [ ] Parse `decmpfs_header`: magic, compression_type, uncompressed_size
- [ ] Implement LZVN decoder:
  - [ ] Handle opcodes: literal, match (distance + length), end-of-stream
  - [ ] Max block size: 65,536 bytes — reject larger as corrupt
- [ ] Implement LZFSE decoder:
  - [ ] Finite State Entropy decoding tables
  - [ ] L/M/D symbol streams
- [ ] Handle inline data: decompress from xattr value directly
- [ ] Handle resource fork: read extent data, decompress block-by-block
- [ ] Wire into `apfs_read()`: transparently decompress on read
- [ ] Commit: `"apfs: lzvn lzfse decompression"`

---

## Priority Order

| ⭐ | Priority | Section                        | Description                                                        |
| -- | -------- | ------------------------------ | ------------------------------------------------------------------ |
| 💎 | 🔴 P0   | 1.1 Container Superblock       | Foundation — parse block 0 bootstrap                               |
| 💎 | 🔴 P0   | 1.2 Fletcher-64 Checksum       | Integrity — required for all metadata validation                   |
| 💎 | 🔴 P0   | 1.3 Checkpoint Discovery       | Safety — find actual active superblock (block 0 is stale)          |
| 💎 | 🔴 P0   | 2.1 Object Map (OMAP)          | Translation — virtual OID → physical block                         |
| 💎 | 🔴 P0   | 2.2 B-Tree Engine              | Foundation — generic B-tree used by all metadata                   |
| 💎 | 🟠 P1   | 3.1 Volume Superblock          | Volume — mount individual volumes                                  |
| 💎 | 🟠 P1   | 3.2 Volume OMAP + Catalog      | Volume — set up catalog for metadata lookups                       |
| 💎 | 🟠 P1   | 4.1 Inode Reader               | Metadata — read file/directory metadata                            |
| 💎 | 🟠 P1   | 4.2 Directory Records          | Directory — enumerate and lookup entries                           |
| 💎 | 🟠 P1   | 4.3 Path Resolution            | Directory — resolve full paths                                     |
| 💎 | 🟠 P1   | 5.1 File Extent Reader         | Core — map file offsets to physical blocks                         |
| 💎 | 🟠 P1   | 5.2 File Data Reader           | Core — actually read file contents                                 |
| 💎 | 🟠 P1   | 6.1 VFS Registration           | Integration — make APFS mountable                                  |
| 💎 | 🟡 P2   | 8.1 Space Manager              | Reporting — free space for Disk Manager                            |
| 💎 | 🟡 P2   | 9.1 OMAP + Dentry Cache        | Performance — avoid I/O amplification                              |
| 💎 | 🟡 P2   | 11.1 LZVN/LZFSE Decompression  | Correctness — prevent silent data corruption on compressed files   |
| 💎 | 🟢 P3   | 7.1 Test Suite                 | Quality — automated validation                                     |
| ⭐ | 🟢 P3   | 10.1 Health Dashboard          | **Container-level health** — no OS shows this 🚀                   |
| ⭐ | 🟢 P3   | 10.2 Clone Detective           | **Clone relationship mapping** — unique forensic feature 🚀        |
| ⭐ | 🟢 P3   | 10.3 Snapshot Explorer          | **GUI snapshot browser** — macOS is CLI-only 🚀                   |
| ⭐ | 🟢 P3   | 10.4 Timestamp Inspector       | **Nanosecond timestamp viewer** — macOS Finder shows seconds 🚀    |
| 🔵 | 🔵 P4   | Write support + CoW            | Full R/W — future stretch goal                                     |
| 🔵 | 🔵 P4   | Encryption (AES-XTS + keybags) | Decrypt encrypted volumes — future stretch goal                    |
| 🔵 | 🔵 P4   | TRIM/DEALLOCATE                | SSD optimization — future stretch goal                             |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to macOS, Windows, and Linux.

---

## OS Comparison

| Feature                            | 🍎 macOS (native)                  | 🪟 Windows 11                     | 🐧 Linux                           | 🚀 Impossible OS                         |
| ---------------------------------- | ---------------------------------- | --------------------------------- | ----------------------------------- | ---------------------------------------- |
| Container superblock               | ✅ Full R/W                        | ⚠️ Paragon (paid, R/W)            | ⚠️ apfs-fuse (experimental RO)      | ⬜ §1.1 P0                               |
| Fletcher-64 checksum               | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §1.2 P0                               |
| Checkpoint system                  | ✅ Full crash recovery             | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §1.3 P0                               |
| Object Map (OMAP)                  | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §2.1 P0                               |
| B-tree traversal                   | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §2.2 P0                               |
| Volume mounting                    | ✅ Full multi-volume               | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §3.1 P1                               |
| Inode reading                      | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §4.1 P1                               |
| Directory listing                  | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §4.2 P1                               |
| File reading                       | ✅ Full                            | ⚠️ Paragon                        | ⚠️ apfs-fuse                        | ⬜ §5.2 P1                               |
| Space Manager                      | ✅ Full                            | ❌                                 | ❌                                   | ⬜ §8.1 P2                               |
| LZVN/LZFSE decompression           | ✅ Full                            | ⚠️ Paragon (partial)              | ❌ apfs-fuse (no)                    | ⬜ §11.1 P2                              |
| Encryption (AES-XTS)               | ✅ Full + Secure Enclave           | ⚠️ Paragon (partial, no T2)       | ❌                                   | ⬜ Future P4                              |
| Write support                      | ✅ Full                            | ⚠️ Paragon (paid)                 | ⚠️ linux-apfs-rw (experimental)     | ⬜ Future P3                              |
| Snapshots                          | ✅ Time Machine + CLI              | ❌                                 | ❌                                   | ⬜ §10.3 P3                              |
| Cloning                            | ✅ Zero-cost via Finder            | ❌                                 | ❌                                   | ⬜ Future P3                              |
| **Container health dashboard**     | ❌ `diskutil` CLI only             | ❌ No APFS support                 | ❌ No health reporting               | ⬜ §10.1 P3 — **GUI health panel** 🚀    |
| **Clone detective**                | ❌ No clone visibility             | ❌                                 | ❌                                   | ⬜ §10.2 P3 — **clone family view** 🚀   |
| **Snapshot explorer**              | ❌ CLI-only (`tmutil`)             | ❌                                 | ❌                                   | ⬜ §10.3 P3 — **GUI browser** 🚀         |
| **Nanosecond timestamps**          | ⚠️ Finder shows seconds only       | ❌                                 | ❌                                   | ⬜ §10.4 P3 — **full ns precision** 🚀   |

> **After P0+P1 items:** Impossible OS can read any unencrypted APFS volume — built-in,
> free, and more reliable than third-party Windows/Linux tools.
> **After P2–P3 exclusive features:** Exceeds all three OSes — container health, clone
> detective, snapshot explorer, and nanosecond timestamps are unique to Impossible OS.
> **After P4 items:** Full spec parity including encryption and write support.

---

## Key Files

| File                                   | Purpose                                                               |
| -------------------------------------- | --------------------------------------------------------------------- |
| `src/kernel/fs/apfs.c`                 | [NEW] APFS driver implementation                                      |
| `include/kernel/fs/apfs.h`             | [NEW] APFS structures, constants, object types                        |
| `src/kernel/fs/partition.c`            | Add GPT GUID `7C3457EF-0000-11AA-AA11-00306543ECAC` detection         |
| `include/kernel/fs/partition.h`        | Add `PART_FS_APFS` constant                                          |
| `src/kernel/fs/gpt.c`                  | Add `GPT_GUID_APFS` constant                                         |
| `specs/storage/filesystems/apfs.md`    | Full on-disk specification reference (848 lines)                      |
