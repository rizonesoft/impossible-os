# APFS — Technical Specification for OS Implementation

## Overview and Architectural Context

Apple File System (APFS) is a proprietary, 64-bit, copy-on-write (CoW) file system designed by Apple as the
successor to HFS+. It is optimized for solid-state drives (SSDs) while maintaining backward compatibility with
traditional hard disk drives (HDDs). APFS uses a containerized storage model where multiple volumes dynamically
share a single pool of free space, eliminating rigid per-volume partition boundaries.

Key architectural features:

- **64-bit inode namespace** — supports up to 2^63 files per volume
- **Nanosecond timestamp precision** — replaces HFS+'s one-second resolution
- **Copy-on-Write (CoW)** — modified blocks are written to new locations, never overwritten in place
- **Native encryption** — AES-XTS whole-volume and per-file encryption with hierarchical key management
- **Instant cloning** — zero-cost file and directory clones via metadata-only operations
- **Point-in-time snapshots** — read-only volume snapshots without data duplication
- **Transparent compression** — LZVN, LZFSE, and Deflate via the `decmpfs` framework
- **Dynamic space sharing** — volumes within a container share free space on demand

All multi-byte fields are stored in **little-endian** byte order unless explicitly noted otherwise.

### Version History and Compatibility

| Version     | Date       | Platform              | Key Additions                                        |
| ----------- | ---------- | --------------------- | ---------------------------------------------------- |
| Preview     | 2016-06    | macOS Sierra (dev)    | First developer preview for data drives              |
| 1.0 (iOS)   | 2017-03-27 | iOS 10.3              | Default filesystem for iOS devices                   |
| 1.0 (macOS) | 2017-09-25 | macOS 10.13 High Sierra| Default for SSDs; HFS+ retained for HDDs/Fusion      |
| 2.0         | 2019-10    | macOS 10.15 Catalina  | Firmlinks, read-only system volume, sealed volumes    |
| 3.0         | 2020-11    | macOS 11 Big Sur       | Signed system volume (SSV), sealed snapshot hashes    |
| 4.0         | 2022-10    | macOS 13 Ventura      | Fusion Drive APFS support finalized                  |

> [!NOTE]
> Apple publishes the "Apple File System Reference" document detailing on-disk structures for read-only
> access. This spec synthesizes that reference with open-source implementations (`apfs-fuse`, `linux-apfs`)
> and forensic analysis sources.

---

## Partition Identification and Detection

APFS containers are identified via the GUID Partition Table (GPT).

| Property           | Value                                          |
| ------------------ | ---------------------------------------------- |
| GPT Type GUID      | `7C3457EF-0000-11AA-AA11-00306543ECAC`         |
| Container magic    | `NXSB` (`0x4253584E` little-endian at offset `0x20`) |
| Volume magic       | `APSB` (`0x42535041` little-endian)            |
| Default block size | 4096 bytes (matches NAND flash page size)      |
| Byte order         | Little-endian universally                      |

> [!IMPORTANT]
> The `nx_uuid` field in the container superblock is stored in **big-endian** format, unlike all other
> multi-byte fields in APFS. This is the only exception to the little-endian rule.

### Detection Sequence

1. Parse GPT to locate partition with type GUID `7C3457EF-0000-11AA-AA11-00306543ECAC`
2. Read block 0 of the partition (4096 bytes)
3. Validate the object header checksum (Fletcher-64)
4. Verify `nx_magic` at offset `0x20` equals `0x4253584E` (`"NXSB"`)
5. Read `nx_block_size` at offset `0x24` to determine the container's logical block size

---

## Universal Object Header (`obj_phys_t`)

Every APFS metadata object begins with a 32-byte header that provides integrity verification, transaction
tracking, and type identification.

```c
typedef struct obj_phys {
    uint64_t o_cksum;    /* offset 0x00: Fletcher-64 checksum                    */
    uint64_t o_oid;      /* offset 0x08: Object Identifier                       */
    uint64_t o_xid;      /* offset 0x10: Transaction Identifier                  */
    uint32_t o_type;     /* offset 0x18: Object type + storage type flags         */
    uint32_t o_subtype;  /* offset 0x1C: Subtype (B-tree payload type indicator)  */
} obj_phys_t;            /* total: 32 bytes                                      */
```

| Field      | Offset | Size | Description                                                       |
| ---------- | ------ | ---- | ----------------------------------------------------------------- |
| `o_cksum`  | 0x00   | 8    | Fletcher-64 checksum of bytes 0x08 through end of block           |
| `o_oid`    | 0x08   | 8    | Virtual OID (sequence number) or physical block address           |
| `o_xid`    | 0x10   | 8    | Transaction ID — monotonically increasing per commit              |
| `o_type`   | 0x18   | 4    | Bits 15:0 = base type; bits 31:16 = storage type flags            |
| `o_subtype`| 0x1C   | 4    | Payload subtype for B-tree nodes (e.g., catalog vs OMAP records)  |

### Fletcher-64 Checksum Algorithm

The checksum covers the entire block **excluding** the first 8 bytes (the `o_cksum` field itself).
APFS uses a modified Fletcher-64 with modular arithmetic:

1. Initialize two 32-bit accumulators: `sum1 = 0`, `sum2 = 0`
2. Process the block data (bytes 8 through block_size-1) as an array of 32-bit words
3. For each word: `sum1 = (sum1 + word) mod 0xFFFFFFFF`, `sum2 = (sum2 + sum1) mod 0xFFFFFFFF`
4. Compute `check1 = 0xFFFFFFFF - ((sum1 + sum2) mod 0xFFFFFFFF)`
5. Compute `check2 = 0xFFFFFFFF - ((sum1 + check1) mod 0xFFFFFFFF)`
6. Store `(check2 << 32) | check1` as the 64-bit checksum

> [!CAUTION]
> The Fletcher-64 variant used by APFS differs from the standard RFC 1146 Fletcher checksum.
> The modulus is `0xFFFFFFFF` (not `0xFFFF`), and the final step computes complementary values.
> Using a standard Fletcher-64 implementation will produce incorrect checksums.

### Object Type Constants

Apply `OBJECT_TYPE_MASK` (`0x0000FFFF`) to extract the base type from `o_type`:

| Constant                       | Value    | Structure              | Description                        |
| ------------------------------ | -------- | ---------------------- | ---------------------------------- |
| `OBJECT_TYPE_NX_SUPERBLOCK`    | `0x0001` | `nx_superblock_t`      | Container Superblock               |
| `OBJECT_TYPE_BTREE`            | `0x0002` | `btree_node_phys_t`    | B-tree root node                   |
| `OBJECT_TYPE_BTREE_NODE`       | `0x0003` | `btree_node_phys_t`    | B-tree internal or leaf node       |
| `OBJECT_TYPE_SPACEMAN`         | `0x0005` | `spaceman_phys_t`      | Space Manager                      |
| `OBJECT_TYPE_CAB`              | `0x0006` | —                      | Chunk-Info Address Block           |
| `OBJECT_TYPE_CIB`              | `0x0007` | —                      | Chunk-Info Block                   |
| `OBJECT_TYPE_SPACEMAN_FREE_QUEUE` | `0x0008` | —                   | Free-space queue                   |
| `OBJECT_TYPE_OMAP`             | `0x000B` | `omap_phys_t`          | Object Map                         |
| `OBJECT_TYPE_CHECKPOINT_MAP`   | `0x000C` | `checkpoint_map_phys_t`| Checkpoint mapping                 |
| `OBJECT_TYPE_FS`               | `0x000D` | `apfs_superblock_t`    | Volume Superblock                  |
| `OBJECT_TYPE_FSTREE`           | `0x000E` | `btree_node_phys_t`    | File-system catalog tree (subtype) |
| `OBJECT_TYPE_BLOCKREFTREE`     | `0x000F` | `btree_node_phys_t`    | Extent reference tree (subtype)    |
| `OBJECT_TYPE_SNAPMETATREE`     | `0x0010` | `btree_node_phys_t`    | Snapshot metadata tree (subtype)   |
| `OBJECT_TYPE_REAPER`           | `0x0011` | `reaper_phys_t`        | Object Reaper                      |
| `OBJECT_TYPE_REAP_LIST`        | `0x0012` | —                      | Reaper list                        |

### Object Storage Types

Apply `OBJECT_TYPE_FLAGS_MASK` (`0xFFFF0000`) to extract the storage type from `o_type`:

| Flag              | Value        | Description                                              |
| ----------------- | ------------ | -------------------------------------------------------- |
| `OBJ_VIRTUAL`     | `0x00000000` | Default; OID is a virtual identifier, requires OMAP      |
| `OBJ_PHYSICAL`    | `0x40000000` | OID = physical block address; no OMAP lookup needed      |
| `OBJ_EPHEMERAL`   | `0x80000000` | Lives only in checkpoint data; reconstructed on mount    |

> [!IMPORTANT]
> Physical objects use their block address as their OID. Virtual objects require an Object Map lookup
> to resolve their OID to a physical block address. Ephemeral objects exist only in the checkpoint
> area and are rebuilt during mount.

---

## Container Superblock (`nx_superblock_t`)

The Container Superblock (CSB) is the absolute entry point for the file system. It resides at
**block 0** of the APFS partition but serves only as a bootstrap — the driver must scan the
checkpoint area to find the latest valid superblock.

```c
typedef struct nx_superblock {
    obj_phys_t nx_o;                    /* offset 0x00: Standard object header (32 bytes)  */
    uint32_t   nx_magic;                /* offset 0x20: Must equal 0x4253584E ("NXSB")     */
    uint32_t   nx_block_size;           /* offset 0x24: Logical block size (typically 4096) */
    uint64_t   nx_block_count;          /* offset 0x28: Total blocks in the container       */
    uint64_t   nx_features;             /* offset 0x30: Compatible feature flags             */
    uint64_t   nx_readonly_compatible_features; /* offset 0x38: RO-compatible features      */
    uint64_t   nx_incompatible_features;/* offset 0x40: Incompatible features                */
    uint8_t    nx_uuid[16];             /* offset 0x48: Container UUID (BIG-ENDIAN)          */
    uint64_t   nx_next_oid;             /* offset 0x58: Next available Object ID             */
    uint64_t   nx_next_xid;             /* offset 0x60: Next available Transaction ID        */
    uint32_t   nx_xp_desc_blocks;       /* offset 0x68: Checkpoint descriptor area size      */
    uint32_t   nx_xp_data_blocks;       /* offset 0x6C: Checkpoint data area size            */
    uint64_t   nx_xp_desc_base;         /* offset 0x70: Descriptor area base block address   */
    uint64_t   nx_xp_data_base;         /* offset 0x78: Data area base block address          */
    uint32_t   nx_xp_desc_next;         /* offset 0x80: Next descriptor index                 */
    uint32_t   nx_xp_data_next;         /* offset 0x84: Next data index                       */
    uint32_t   nx_xp_desc_index;        /* offset 0x88: Current descriptor index              */
    uint32_t   nx_xp_desc_len;          /* offset 0x8C: Active descriptor count               */
    uint32_t   nx_xp_data_index;        /* offset 0x90: Current data index                    */
    uint32_t   nx_xp_data_len;          /* offset 0x94: Active data count                     */
    uint64_t   nx_spaceman_oid;         /* offset 0x98: Space Manager virtual OID             */
    uint64_t   nx_omap_oid;             /* offset 0xA0: Container Object Map physical addr    */
    uint64_t   nx_reaper_oid;           /* offset 0xA8: Reaper virtual OID                    */
    uint32_t   nx_test_type;            /* offset 0xB0: Reserved for testing                  */
    uint32_t   nx_max_file_systems;     /* offset 0xB4: Maximum volumes (default 100)         */
    uint64_t   nx_fs_oid[100];          /* offset 0xB8: Volume superblock virtual OIDs        */
    /* ... additional fields follow (counters, Fusion, keybag location) */
} nx_superblock_t;
```

> [!CAUTION]
> The block 0 superblock is only a bootstrap copy. It may be stale. The driver **must** scan the
> checkpoint descriptor area to find the superblock with the highest valid `o_xid`. Using the
> block 0 copy for active state tracking will cause data loss.

---

## Checkpoint System and Crash Recovery

APFS achieves crash safety through atomic checkpoints combined with CoW. A checkpoint captures
the complete, consistent state of all container metadata at a specific transaction.

### Checkpoint Areas

The container maintains two circular ring buffers:

| Area                | Base Field          | Size Field            | Contents                        |
| ------------------- | ------------------- | --------------------- | ------------------------------- |
| Descriptor Area     | `nx_xp_desc_base`   | `nx_xp_desc_blocks`   | Superblock copies + mapping     |
| Data Area           | `nx_xp_data_base`   | `nx_xp_data_blocks`   | Ephemeral object snapshots      |

### Checkpoint Discovery Algorithm

1. Read the bootstrap superblock from block 0
2. Extract `nx_xp_desc_base` and `nx_xp_desc_blocks`
3. Scan each block in the descriptor area ring buffer:
   a. Validate Fletcher-64 checksum
   b. Check `o_type & OBJECT_TYPE_MASK` for `OBJECT_TYPE_NX_SUPERBLOCK` (`0x0001`)
   c. Track the block with the highest valid `o_xid`
4. The block with the highest `o_xid` is the **active container superblock**
5. From the active CSB, locate the Checkpoint Map (`OBJECT_TYPE_CHECKPOINT_MAP`, `0x000C`)
6. Parse `checkpoint_mapping_t` entries to locate all ephemeral objects
7. Stop when an entry with `CHECKPOINT_MAP_LAST` flag (`0x00000001`) is encountered

```c
typedef struct checkpoint_mapping {
    uint32_t cpm_type;       /* Object type                                 */
    uint32_t cpm_subtype;    /* Object subtype                              */
    uint32_t cpm_size;       /* Size in bytes                               */
    uint32_t cpm_pad;        /* Padding                                     */
    uint64_t cpm_fs_oid;     /* File system OID (0 for container objects)   */
    uint64_t cpm_oid;        /* Object virtual OID                          */
    uint64_t cpm_paddr;      /* Physical block address in data area          */
} checkpoint_mapping_t;      /* 40 bytes                                    */
```

> [!IMPORTANT]
> If a power failure interrupts checkpoint writing, the incomplete checkpoint will have an invalid
> Fletcher-64 checksum. The driver safely ignores it and rolls back to the last complete checkpoint.
> This is the fundamental crash safety guarantee of APFS.

---

## Object Map (`omap_phys_t`)

The Object Map (OMAP) is the central translation layer between virtual OIDs and physical block
addresses. It eliminates cascading pointer updates during CoW operations.

```c
typedef struct omap_phys {
    obj_phys_t om_o;            /* Standard object header                         */
    uint32_t   om_flags;        /* OMAP flags                                     */
    uint32_t   om_snap_count;   /* Number of snapshots using this OMAP             */
    uint32_t   om_tree_type;    /* B-tree type for the mapping tree                */
    uint32_t   om_snapshot_tree_type; /* B-tree type for snapshot tree             */
    uint64_t   om_tree_oid;     /* Physical OID of the mapping B-tree root         */
    uint64_t   om_snapshot_tree_oid;  /* Physical OID of snapshot B-tree root      */
    uint64_t   om_most_recent_snap;   /* Most recent snapshot transaction ID       */
    uint64_t   om_pending_revert_min; /* Minimum pending revert XID                */
    uint64_t   om_pending_revert_max; /* Maximum pending revert XID                */
} omap_phys_t;
```

### OMAP B-Tree Key and Value

```c
typedef struct omap_key {
    uint64_t ok_oid;    /* Virtual Object Identifier     */
    uint64_t ok_xid;    /* Transaction Identifier        */
} omap_key_t;           /* 16 bytes                      */

typedef struct omap_val {
    uint32_t ov_flags;  /* Flags (e.g., OMAP_VAL_ENCRYPTED) */
    uint32_t ov_size;   /* Object size in bytes              */
    uint64_t ov_paddr;  /* Physical block address            */
} omap_val_t;           /* 16 bytes                          */
```

### OMAP Lookup Process

1. Extract the virtual OID from the object reference
2. Search the OMAP B-tree for the key `(oid, xid)` where `xid` ≤ current transaction
3. For exact matches: return `ov_paddr`
4. For snapshot access: use the highest `xid` ≤ snapshot's transaction ID

> [!CAUTION]
> Every virtual object read requires an OMAP lookup. The driver **must** cache OMAP B-tree branch
> nodes in RAM. Failing to do so incurs O(log N) disk reads per file access, causing catastrophic
> I/O amplification on volumes with millions of files.

---

## Space Manager (`spaceman_phys_t`)

The Space Manager tracks free/used blocks across the entire container using a hierarchical
bitmap structure. All volumes share this single allocator.

### Bitmap Hierarchy

```
Space Manager (spaceman_phys_t)
  └── Chunk-Info Address Blocks (CABs, type 0x0006)
        └── Chunk-Info Blocks (CIBs, type 0x0007)
              └── Free-Space Bitmaps (raw bitmap blocks)
```

| Field                | Description                                              |
| -------------------- | -------------------------------------------------------- |
| `sm_block_size`      | Matches container block size (typically 4096)            |
| `sm_blocks_per_chunk`| Number of blocks tracked per bitmap chunk                |
| `sm_chunks_per_cib`  | Number of chunks managed by one Chunk-Info Block          |
| `sm_cibs_per_cab`    | Number of CIBs managed by one Chunk-Info Address Block    |
| `sm_ip_block_count`  | Blocks dedicated to internal pool management              |

### Bitmap Encoding

Each bit in the free-space bitmap represents one logical block:

- **Bit = 1**: Block is allocated (in use)
- **Bit = 0**: Block is free

One bitmap byte tracks 8 blocks. Bitmap blocks themselves are allocated contiguously.

### The Reaper (`reaper_phys_t`)

Large deletion operations (volume removal, huge file deletion) are handled asynchronously by the
Reaper to avoid stalling the system in a single transaction:

1. Objects queued for deletion are registered with the Reaper
2. The Reaper incrementally frees extent blocks across multiple transactions
3. Space Manager bitmap bits are cleared as blocks are released
4. Container maintains exactly one Reaper instance

---

## Volume Superblock (`apfs_superblock_t`)

Each volume within a container has its own superblock, accessed by resolving the virtual OID
from `nx_fs_oid[]` through the container's Object Map.

```c
typedef struct apfs_superblock {
    obj_phys_t apfs_o;                      /* Standard object header              */
    uint32_t   apfs_magic;                  /* Must equal 0x42535041 ("APSB")      */
    uint32_t   apfs_fs_index;               /* Index in container's volume array   */
    uint64_t   apfs_features;               /* Compatible feature flags            */
    uint64_t   apfs_readonly_compatible_features;
    uint64_t   apfs_incompatible_features;  /* Incompatible feature flags          */
    uint64_t   apfs_unmount_time;           /* Last unmount timestamp (nanoseconds)*/
    uint64_t   apfs_fs_reserve_block_count; /* Reserved blocks for this volume     */
    uint64_t   apfs_fs_quota_block_count;   /* Maximum blocks this volume may use  */
    uint64_t   apfs_fs_alloc_count;         /* Currently allocated block count     */
    /* ... crypto state, root tree OID, etc. */
    uint64_t   apfs_omap_oid;               /* Volume Object Map physical OID      */
    uint64_t   apfs_root_tree_oid;          /* Catalog B-tree root virtual OID     */
    uint64_t   apfs_extentref_tree_oid;     /* Extent reference tree virtual OID   */
    uint64_t   apfs_snap_meta_tree_oid;     /* Snapshot metadata tree virtual OID  */
    /* ... additional fields */
} apfs_superblock_t;
```

### Volume Flags

| Flag                              | Value        | Description                                 |
| --------------------------------- | ------------ | ------------------------------------------- |
| `APFS_FS_UNENCRYPTED`            | `0x00000001` | Volume uses no encryption                   |
| `APFS_FS_EFFACEABLE`             | `0x00000002` | Volume supports secure erase                |
| `APFS_FS_RESERVED_4`             | `0x00000004` | Reserved                                    |
| `APFS_FS_ONEKEY`                 | `0x00000008` | Single software encryption key for volume   |
| `APFS_FS_SPILLEDOVER`            | `0x00000010` | Volume has exceeded its space allocation    |
| `APFS_FS_RUN_SPILLOVER_CLEANER`  | `0x00000020` | Spillover cleaner must run on next mount    |

### Volume Incompatible Features

| Flag                                          | Value        | Description                          |
| --------------------------------------------- | ------------ | ------------------------------------ |
| `APFS_INCOMPAT_CASE_INSENSITIVE`              | `0x00000001` | Case-insensitive filename matching   |
| `APFS_INCOMPAT_DATALESS_SNAPS`                | `0x00000002` | Dataless snapshots                   |
| `APFS_INCOMPAT_ENC_ROLLED`                    | `0x00000004` | Encryption keys have been rolled     |
| `APFS_INCOMPAT_NORMALIZATION_INSENSITIVE`     | `0x00000008` | Unicode normalization-insensitive    |

---

## B-Tree Architecture (`btree_node_phys_t`)

APFS uses B+ trees exclusively for all metadata storage. Leaf nodes hold actual key-value data;
branch (internal) nodes hold keys and child pointers only.

### Node Structure

```c
typedef struct btree_node_phys {
    obj_phys_t btn_o;           /* Standard object header                          */
    uint16_t   btn_flags;       /* Node flags                                      */
    uint16_t   btn_level;       /* 0 = leaf; >0 = branch (levels below this node)  */
    uint32_t   btn_nkeys;       /* Number of keys in this node                     */
    nloc_t     btn_table_space; /* Offset and length of the Table of Contents      */
    nloc_t     btn_free_space;  /* Offset and length of free space region           */
    nloc_t     btn_key_free_list;  /* Free list for key area                        */
    nloc_t     btn_val_free_list;  /* Free list for value area                      */
    /* Table of Contents entries follow                                             */
    /* Key data grows forward from TOC end                                          */
    /* Value data grows backward from block end                                     */
    /* If BTNODE_ROOT: btree_info_t appended at block end                           */
} btree_node_phys_t;
```

### Node Flags (`btn_flags`)

| Flag                  | Value    | Description                                          |
| --------------------- | -------- | ---------------------------------------------------- |
| `BTNODE_ROOT`         | `0x0001` | Node is the root of the B-tree                       |
| `BTNODE_LEAF`         | `0x0002` | Node is a leaf (holds actual key-value data)         |
| `BTNODE_FIXED_KV_SIZE`| `0x0004` | Keys and values have fixed sizes (TOC uses 4-byte entries) |
| `BTNODE_HASHED`       | `0x0010` | Node uses hashed keys                                |
| `BTNODE_NOHEADER`     | `0x0020` | Node has no object header                            |

### Bidirectional Memory Layout

Within each B-tree node block, data grows in opposite directions:

```
+-------------------------------------------------------------------+
| obj_phys_t | btree_node_phys_t | TOC | Keys →  ... ← Values      |
|            |                   |     |    free space    |          |
+-------------------------------------------------------------------+
                                       ↑                 ↑
                                    key_end          val_start
```

- **Table of Contents (TOC)**: Array of entry descriptors immediately after the node header
- **Keys**: Grow forward (toward higher addresses) from the end of the TOC
- **Values**: Grow backward (toward lower addresses) from the end of the block
- **Free space**: Central gap between keys and values

For fixed-size entries (`BTNODE_FIXED_KV_SIZE`): TOC entry = 2-byte key offset + 2-byte value offset.
For variable-size entries: TOC entry = 2-byte key offset + 2-byte key length + 2-byte value offset.

### Root Node Metadata (`btree_info_t`)

If `BTNODE_ROOT` is set, a `btree_info_t` structure is appended at the end of the block:

```c
typedef struct btree_info {
    btree_info_fixed_t bt_fixed;    /* Fixed tree metadata                   */
    uint32_t           bt_longest_key;  /* Maximum key length in the tree    */
    uint32_t           bt_longest_val;  /* Maximum value length in the tree  */
    uint64_t           bt_key_count;    /* Total keys across all nodes       */
    uint64_t           bt_node_count;   /* Total nodes in the tree           */
} btree_info_t;
```

---

## File-System Catalog Records

The Catalog B-tree (subtype `OBJECT_TYPE_FSTREE`, `0x000E`) stores all file and directory
metadata as key-value pairs. Records are sorted by: Object ID → record type → name.

### Catalog Key (`j_key_t`)

```c
typedef struct j_key {
    uint64_t obj_id_and_type;   /* Bits 63:60 = record type; bits 59:0 = Object ID */
} j_key_t;                      /* 8 bytes                                          */
```

| Mask / Shift       | Value            | Purpose                              |
| ------------------ | ---------------- | ------------------------------------ |
| `OBJ_ID_MASK`      | `0x0FFFFFFFFFFFFFFF` | Extract Object ID (bits 59:0)    |
| `OBJ_TYPE_MASK`    | `0xF000000000000000` | Extract record type (bits 63:60) |
| `OBJ_TYPE_SHIFT`   | `60`             | Right-shift to get type value        |

### Catalog Record Types

| Type Constant         | Value  | Description                            |
| --------------------- | ------ | -------------------------------------- |
| `APFS_TYPE_SNAP_METADATA` | `0x1` | Snapshot metadata                   |
| `APFS_TYPE_EXTENT`    | `0x2`  | Physical extent record                 |
| `APFS_TYPE_INODE`     | `0x3`  | Inode (file/directory metadata)        |
| `APFS_TYPE_XATTR`     | `0x4`  | Extended attribute                     |
| `APFS_TYPE_SIBLING_LINK` | `0x5` | Sibling link (hard link tracking)   |
| `APFS_TYPE_DSTREAM_ID` | `0x6` | Data-stream identifier                 |
| `APFS_TYPE_CRYPTO_STATE`| `0x7` | Per-file crypto state                 |
| `APFS_TYPE_FILE_EXTENT` | `0x8` | File extent (data location)           |
| `APFS_TYPE_DIR_REC`   | `0x9`  | Directory entry record                 |
| `APFS_TYPE_DIR_STATS` | `0xA`  | Directory statistics                   |
| `APFS_TYPE_SNAP_NAME` | `0xB`  | Snapshot name                          |
| `APFS_TYPE_SIBLING_MAP`| `0xC` | Sibling map                            |

---

## Inode Records (`APFS_TYPE_INODE`, `0x3`)

### Inode Value (`j_inode_val_t`)

```c
typedef struct j_inode_val {
    uint64_t parent_id;             /* Parent directory Object ID                  */
    uint64_t private_id;            /* Data stream Object ID                       */
    uint64_t create_time;           /* Creation time (ns since Unix epoch)          */
    uint64_t mod_time;              /* Modification time (ns since Unix epoch)      */
    uint64_t change_time;           /* Attribute change time (ns since Unix epoch)  */
    uint64_t access_time;           /* Last access time (ns since Unix epoch)       */
    uint64_t internal_flags;        /* APFS-specific behavioral flags              */
    union {
        int32_t nchildren;          /* Directory: number of child entries           */
        int32_t nlink;              /* File: hard link count                        */
    };
    uint32_t default_protection_class; /* iOS encryption protection class           */
    uint32_t write_generation_counter; /* Write generation counter                 */
    uint32_t bsd_flags;             /* BSD file flags (UF_HIDDEN, etc.)             */
    uint32_t owner;                 /* User ID (UID)                                */
    uint32_t group;                 /* Group ID (GID)                               */
    uint16_t mode;                  /* POSIX file mode and permissions              */
    uint16_t pad1;                  /* Padding                                      */
    uint64_t uncompressed_size;     /* Uncompressed size (if compressed)            */
    /* Extended fields (xf_blob_t) may follow */
} j_inode_val_t;
```

### Inode Internal Flags

| Flag                          | Value        | Description                                    |
| ----------------------------- | ------------ | ---------------------------------------------- |
| `INODE_IS_APFS_PRIVATE`       | `0x00000001` | Internal APFS object, not user-visible          |
| `INODE_MAINTAIN_DIR_STATS`    | `0x00000002` | Maintain `j_dir_stats_val_t` for fast dir size  |
| `INODE_DIR_STATS_ORIGIN`      | `0x00000004` | Origin of directory statistics tracking         |
| `INODE_PROT_CLASS_EXPLICIT`   | `0x00000008` | Protection class explicitly set                 |
| `INODE_WAS_CLONED`            | `0x00000010` | Inode was created via clone operation            |
| `INODE_HAS_SECURITY_EA`       | `0x00000020` | Has security extended attribute                 |
| `INODE_BEING_TRUNCATED`       | `0x00000040` | Currently being truncated                       |
| `INODE_HAS_FINDER_INFO`       | `0x00000080` | Has Finder info extended attribute              |
| `INODE_IS_SPARSE`             | `0x00000200` | File is sparse (has holes)                      |
| `INODE_WAS_EVER_CLONED`       | `0x00000400` | File was cloned at some point                   |
| `INODE_HAS_UNCOMPRESSED_SIZE` | `0x00000800` | `uncompressed_size` field is valid              |
| `INODE_IS_PURGEABLE`          | `0x00001000` | May be deleted by spillover cleaner             |

### POSIX Mode Masks

| Mask      | Value      | Description     |
| --------- | ---------- | --------------- |
| `S_IFMT`  | `0170000`  | File type mask  |
| `S_IFDIR` | `0040000`  | Directory       |
| `S_IFREG` | `0100000`  | Regular file    |
| `S_IFLNK` | `0120000`  | Symbolic link   |

### Extended Fields (`xf_blob_t`)

If the inode value size exceeds the base `j_inode_val_t`, trailing bytes are an extended field array:

```c
typedef struct xf_blob {
    uint16_t xf_num_exts;     /* Number of extended field entries   */
    uint16_t xf_used_data;    /* Total bytes of extended field data */
    /* x_field_t entries follow, 8-byte aligned */
} xf_blob_t;

typedef struct x_field {
    uint8_t  x_type;     /* Extended field type       */
    uint8_t  x_flags;    /* Flags                     */
    uint16_t x_size;     /* Data size in bytes         */
    /* Field data follows, padded to 8-byte boundary */
} x_field_t;
```

---

## Directory Records (`APFS_TYPE_DIR_REC`, `0x9`)

### Directory Record Key

**Leaf node format** (`j_drec_key_t`):

```c
typedef struct j_drec_key {
    j_key_t  hdr;             /* obj_id_and_type with parent dir OID + type 0x9 */
    uint16_t name_len;        /* Length of the UTF-8 name including NUL          */
    uint8_t  name[];          /* Variable-length UTF-8 encoded file name        */
} j_drec_key_t;
```

**Branch node format** (`j_drec_hashed_key_t`):

```c
typedef struct j_drec_hashed_key {
    j_key_t  hdr;
    uint32_t name_len_and_hash;  /* Bits 31:22 = hash; bits 21:0 = name length */
    uint8_t  name[];
} j_drec_hashed_key_t;
```

> [!NOTE]
> The 22-bit hash in branch nodes accelerates lookups by allowing fast comparison without full
> string matching. The hash preserves Unicode Form D normalization for case-insensitive comparisons.

### Directory Record Value (`j_drec_val_t`)

```c
typedef struct j_drec_val {
    uint64_t file_id;      /* Target inode Object ID              */
    uint64_t date_added;   /* Timestamp when entry was added (ns) */
    uint16_t flags;        /* Entry flags                         */
    uint8_t  xfields[];    /* Optional extended fields             */
} j_drec_val_t;
```

### Path Resolution Algorithm

To resolve `/Users/admin/file.txt`:

1. Start at the volume's root directory (inode 2, the root inode)
2. Hash `"Users"`, search directory records with parent OID = root OID
3. Extract `file_id` → this is the inode OID for `Users/`
4. Resolve inode via OMAP → load directory records for that OID
5. Hash `"admin"`, search → extract `file_id`
6. Recurse until `"file.txt"` is found
7. Final `file_id` points to the target file's inode

---

## File Extents (`APFS_TYPE_FILE_EXTENT`, `0x8`)

File extents map logical file offsets to physical disk blocks.

### File Extent Key

```c
typedef struct j_file_extent_key {
    j_key_t  hdr;             /* Object ID + type 0x8          */
    uint64_t logical_addr;    /* Logical byte offset in file   */
} j_file_extent_key_t;       /* 16 bytes                      */
```

### File Extent Value (`j_file_extent_val_t`)

```c
typedef struct j_file_extent_val {
    uint64_t len_and_flags;    /* Bits 55:0 = length in bytes; bits 63:56 = flags  */
    uint64_t phys_block_num;   /* Starting physical block number                    */
    uint64_t crypto_id;        /* Encryption tweak value (0 if unencrypted)         */
} j_file_extent_val_t;         /* 24 bytes                                          */
```

| Mask / Constant              | Value                | Description                    |
| ---------------------------- | -------------------- | ------------------------------ |
| `J_FILE_EXTENT_LEN_MASK`     | `0x00FFFFFFFFFFFFFF` | Extract extent length (bits 55:0) |
| `J_FILE_EXTENT_FLAG_MASK`    | `0xFF00000000000000` | Extract flags (bits 63:56)      |

> [!IMPORTANT]
> The extent length in `len_and_flags` is measured in **bytes**, not blocks. It must be a multiple
> of `nx_block_size`. Physical block numbers are relative to the start of the container.

### Physical Extents (`APFS_TYPE_EXTENT`, `0x2`)

Physical extents track global block ownership and reference counts for CoW sharing:

```c
typedef struct j_phys_ext_val {
    uint64_t len_and_kind;    /* Bits 59:0 = length; bits 63:60 = kind  */
    uint64_t owning_obj_id;   /* Owning file's Object ID                */
    int32_t  refcnt;          /* Reference count for CoW sharing         */
} j_phys_ext_val_t;           /* 20 bytes                                */
```

> [!CAUTION]
> Do not confuse file extents (type `0x8`) with physical extents (type `0x2`). Physical extents
> track **reference counts** for cloned/snapshotted blocks. The Space Manager only frees a block
> when its physical extent `refcnt` reaches zero.

---

## Security and Encryption

APFS provides native AES-XTS encryption with a hierarchical key management system.

### Key Hierarchy

```
User Password / Recovery Key / Hardware UID
              ↓
    Key Encryption Key (KEK)
              ↓  (RFC 3394 AES Key Wrap)
    Volume Encryption Key (VEK)
              ↓  (AES-XTS)
    File Extent Data Blocks
```

### Keybag System

| Component         | Location                           | Contents                          |
| ----------------- | ---------------------------------- | --------------------------------- |
| Container Keybag  | `nx_keylocker` (block range in CSB)| Wrapped VEKs for all volumes      |
| Volume Keybag     | Referenced from container keybag   | Wrapped KEKs for this volume      |

### Encryption Unlock Sequence

1. Locate the Container Keybag using `nx_keylocker` from the container superblock
2. Decrypt the keybag using the container UUID as key and tweak
3. User provides password (or hardware Secure Enclave provides UID key)
4. Derive the KEK from the user credential
5. Unwrap the VEK from the keybag using AES Key Wrap per RFC 3394
6. Use VEK as the AES-XTS primary key
7. For each file extent: use `crypto_id` from `j_file_extent_val_t` as the AES-XTS tweak

### Encryption Modes

| Flag                      | Value        | Mode                                    |
| ------------------------- | ------------ | --------------------------------------- |
| `APFS_FS_UNENCRYPTED`     | `0x00000001` | No encryption — plaintext               |
| `APFS_FS_ONEKEY`          | `0x00000008` | Single VEK for entire volume            |
| Per-file (default on iOS) | —            | Each file has its own crypto state (type `0x7`) |

> [!NOTE]
> For a bare-metal OS that only needs read access to unencrypted APFS volumes, the entire
> encryption subsystem can be deferred. Check `APFS_FS_UNENCRYPTED` on the volume flags first.

---

## Transparent Compression (`decmpfs`)

APFS inherits the `decmpfs` (AppleFSCompression) framework from HFS+. Compressed files store
the compression metadata in an extended attribute named `com.apple.decmpfs`.

### Compression Algorithms

| Algorithm | Description                                        | Max Block Size |
| --------- | -------------------------------------------------- | -------------- |
| LZVN      | Apple-proprietary LZ variant; fast decompression   | 65,536 bytes   |
| LZFSE     | Finite State Entropy coding; 2-3x faster than zlib | —              |
| Deflate   | Standard zlib/RFC 1951; legacy fallback            | —              |

### Decompression Detection

1. Check inode for extended attribute `com.apple.decmpfs`
2. Parse the `decmpfs_header` from the xattr value
3. If `compression_type` indicates inline data: decompress from the xattr itself
4. If resource fork compression: read extent data and decompress using the indicated algorithm

> [!CAUTION]
> If the kernel driver lacks the required decompression algorithms, it will return raw compressed
> data to userspace applications, causing **silent data corruption**. At minimum, implement LZVN
> and LZFSE decoders. LZVN blocks exceeding 65,536 bytes indicate corruption and must be rejected.

---

## QEMU Testing Configuration

APFS testing in QEMU requires a pre-formatted APFS disk image. QEMU does not natively create
APFS volumes; use a macOS VM or `mkfs.apfs` (from `apfsprogs`) to prepare images.

### Basic Read-Only Test

```bash
# Create a raw disk image with APFS on macOS first, then:
qemu-system-x86_64 \
    -m 512M \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file=system-disk.img,format=raw,if=none,id=boot \
    -device virtio-blk-pci,drive=boot \
    -drive file=apfs-test.img,format=raw,if=none,id=apfs \
    -device virtio-blk-pci,drive=apfs \
    -serial stdio
```

### Creating a Test Image (on macOS)

```bash
# Create a 256 MB sparse image
hdiutil create -size 256m -fs APFS -volname "TestVol" apfs-test.dmg
# Convert to raw format
hdiutil convert apfs-test.dmg -format UDTO -o apfs-test.cdr
mv apfs-test.cdr apfs-test.img
```

### Alternative: Using apfsprogs (Linux)

```bash
# Build apfsprogs from source (https://github.com/linux-apfs/apfsprogs)
dd if=/dev/zero of=apfs-test.img bs=1M count=256
mkfs.apfs apfs-test.img
```

---

## Implementation Priorities for Impossible OS

| Priority | Component                              | Description                                       |
| -------- | -------------------------------------- | ------------------------------------------------- |
| 🔴 P0    | GPT detection + partition GUID         | Identify APFS partitions during disk enumeration   |
| 🔴 P0    | Container superblock parsing           | Read block 0, validate magic, extract layout       |
| 🔴 P0    | Fletcher-64 checksum                   | Required for all metadata validation               |
| 🔴 P0    | Checkpoint discovery                   | Find latest valid superblock in descriptor ring    |
| 🟠 P1    | Object Map (OMAP)                      | Virtual-to-physical OID translation                |
| 🟠 P1    | B-tree traversal                       | Generic B-tree search for all metadata lookups     |
| 🟠 P1    | Volume superblock parsing              | Mount individual volumes                           |
| 🟠 P1    | Catalog B-tree (read-only)             | Inode, directory record, and extent lookups         |
| 🟠 P1    | File extent resolution                 | Map file offsets to physical blocks for reading     |
| 🟡 P2    | Directory entry caching                | VFS dentry cache for path resolution performance   |
| 🟡 P2    | OMAP branch node pinning               | Cache OMAP tree in RAM to avoid I/O amplification  |
| 🟡 P2    | Space Manager (read-only)              | Free space reporting                               |
| 🟡 P2    | LZVN/LZFSE decompression              | Transparent file decompression                      |
| 🟢 P3    | Write support + CoW                    | Allocate new blocks, update OMAP, flush checkpoints |
| 🟢 P3    | Snapshot support                       | Read-only snapshot access                           |
| 🟢 P3    | Cloning                               | Zero-cost file/directory clones                     |
| 🔵 P4    | Encryption (AES-XTS + keybags)         | Decrypt encrypted volumes                           |
| 🔵 P4    | TRIM/DEALLOCATE support                | SSD optimization for freed blocks                   |
| 🔵 P4    | Directory statistics                   | Fast directory sizing via `j_dir_stats_val_t`       |

> [!NOTE]
> Impossible OS currently has no APFS implementation. The recommended approach is to implement
> read-only support first (P0 + P1), then add write support (P3) once the read path is stable.
> Encryption can be deferred indefinitely for non-macOS use cases.

---

## VFS Integration Requirements

### Mount Sequence

1. Detect APFS partition via GPT type GUID
2. Read and validate container superblock (block 0 → checkpoint scan)
3. Load Space Manager from checkpoint
4. Resolve container OMAP
5. For each volume OID in `nx_fs_oid[]`:
   a. Resolve volume superblock via container OMAP
   b. Validate `apfs_magic` = `0x42535041`
   c. Load volume OMAP
   d. Load catalog B-tree root via volume OMAP
   e. Create VFS superblock and mount the volume

### VFS Operation Mapping

| VFS Operation | APFS Implementation                                         |
| ------------- | ------------------------------------------------------------ |
| `open()`      | Traverse catalog B-tree, resolve inode, create vnode          |
| `read()`      | Lookup file extents, read physical blocks, decompress if needed |
| `readdir()`   | Enumerate directory records with matching parent OID          |
| `stat()`      | Return inode metadata (timestamps, mode, size)                |
| `write()`     | Allocate new blocks (CoW), create new extent, update OMAP     |
| `mkdir()`     | Create inode + directory record in catalog                    |
| `unlink()`    | Remove directory record; if nlink=0, queue for Reaper         |

### Performance Requirements

- **OMAP caching**: Pin upper B-tree levels in RAM (mandatory)
- **Dentry cache**: Cache recent path-to-vnode mappings
- **Write-back alignment**: Flush dirty data in sync with APFS checkpoint commits
- **TRIM dispatch**: Send ATA TRIM / NVMe DEALLOCATE after Space Manager frees blocks

> [!IMPORTANT]
> APFS's CoW model generates heavy write amplification on SSDs. Without TRIM support, write
> performance degrades severely over time (potentially from 500 MB/s to ~100 MB/s on SATA)
> as the SSD runs out of pre-erased NAND pages.
