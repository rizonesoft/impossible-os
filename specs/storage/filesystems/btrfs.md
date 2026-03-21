# Btrfs On-Disk Format and Operating System Integration Specification

## 1. Architectural Foundation and Copy-On-Write Mechanics

The B-tree File System (Btrfs) represents a fundamental paradigm shift from traditional journaling file systems that have dominated operating system storage layers for decades. Designed to implement advanced features while focusing on fault tolerance, repairability, and seamless administration, Btrfs relies entirely on a strict Copy-On-Write (COW) architecture. For engineers implementing Btrfs support within a custom operating system Virtual File System (VFS), understanding this COW mechanic is the mandatory starting point, as it dictates all subsequent memory management and disk I/O operations.

In traditional file systems such as ext4 or XFS, modifying a file involves overwriting the existing data blocks in place on the physical media. Btrfs, conversely, never overwrites data or metadata in place under default operational parameters. Instead, any modification involves writing the new data to a completely new, unallocated block on the storage device. Once the new block is successfully written to the hardware, the file system updates the metadata pointer to reference this new physical location. Because Btrfs structures its entire metadata apparatus as a hierarchy of interconnected B-trees, updating a data pointer in a leaf node necessitates rewriting that entire leaf node to a new location. Consequently, the pointer to that leaf node in its parent internal node must also be updated, forcing the parent to be rewritten as well. This cascading effect, known in computational theory as "path copying" or "wandering trees," propagates all the way up the hierarchy to the root node of the affected tree.

> [!IMPORTANT]
> All Btrfs on-disk data structures are stored in **little-endian** byte order, regardless of the host CPU architecture. The custom OS must employ byte-swapping macros (`le64_to_cpu`, `cpu_to_le64`, etc.) when reading from or writing to the disk on big-endian processor architectures.

### Atomic Transaction Guarantees

This architecture provides inherent atomic transaction capabilities. The file system state is only officially updated when the global superblock is modified to point to the new, fully written root node. If a power failure or kernel panic occurs during a write operation, the old tree remains entirely intact and consistent on the disk, effectively eliminating the need for traditional, time-consuming file system consistency checks (`fsck`) upon system reboot. The file system simply mounts the last known good superblock and resumes operation.

### Writable Snapshots via COW

Furthermore, this foundational COW mechanic enables nearly instantaneous, writable snapshots. A snapshot in Btrfs is not a physical copy of data; it is simply a cloned root node that points to the exact same child nodes as the original subvolume, with the reference counts for those shared blocks incremented. When a block in the live subvolume is subsequently modified, only the modified block is COWed, naturally decoupling the snapshot's state from the live subvolume's state without requiring massive data duplication.

### COW Implementation Challenges

However, implementing Btrfs integration requires navigating significant complexities regarding block allocation, reference counting, and delayed write mechanisms. A naive implementation of COW will lead to rapid and aggressive disk fragmentation. The custom operating system's VFS layer must be engineered to handle delayed allocation natively, caching write requests in memory and maximizing the size of contiguous data extents before finally flushing them to physical media. Furthermore, the system must carefully manage the `NODATACOW` file attribute, which bypasses the COW mechanism for specific workloads like large databases or active swapfiles, both of which degrade severely under constant COW fragmentation.

---

## 2. Global On-Disk Structures and Core Primitives

Before detailing the complex specific data structures, the underlying data representation primitives must be established within the OS driver. Btrfs mandates a strictly little-endian on-disk format for all data types, regardless of the host system's architecture.

> [!CAUTION]
> Standard byte-by-byte memory comparison functions (like `memcmp`) will **fail** to sort Btrfs keys correctly because the most significant bytes are stored last in little-endian representation. The OS driver **must** implement a dedicated key comparison algorithm that evaluates the fields sequentially in memory.

### 2.1 The Btrfs Key (`btrfs_disk_key`)

The universal addressing and sorting mechanism within Btrfs is the `btrfs_disk_key`. Every single item of metadata within the file system is indexed and sorted by a Key. The Key is a 17-byte packed C structure:

| Offset | Size (Bytes) | Type | Description |
|--------|-------------|------|-------------|
| `0x00` | 8 | UINT | The `objectid`. Identifies the logical object (e.g., inode number). |
| `0x08` | 1 | UINT | The `type`. Determines the specific structure of the payload. |
| `0x09` | 8 | UINT | The `offset`. Provides contextual meaning based on the type. |

The `objectid` identifies the logical entity to which the item belongs. For file system trees, this corresponds to the inode number. All items belonging to a specific inode—whether they are extent pointers, directory entries, or extended attributes—will share the same `objectid`, forcing the B-tree sorting algorithm to group them contiguously within the leaf nodes.

> [!NOTE]
> The values are unsigned; an `objectid` of `-1` is treated as `0xFFFFFFFFFFFFFFFF` and will be sorted to the absolute end of the tree.

The `type` field is an 8-bit identifier that dictates how the operating system must cast and parse the payload data associated with the key. Common types include:

| Type Constant | Value | Description |
|--------------|-------|-------------|
| `BTRFS_INODE_ITEM_KEY` | 1 | Inode metadata record |
| `BTRFS_INODE_REF_KEY` | 12 | Inode back-reference (filename + parent dir) |
| `BTRFS_XATTR_ITEM_KEY` | 24 | Extended attribute |
| `BTRFS_DIR_ITEM_KEY` | 84 | Directory entry (keyed by CRC32C hash of name) |
| `BTRFS_DIR_INDEX_KEY` | 96 | Directory index entry (keyed by sequence number) |
| `BTRFS_EXTENT_DATA_KEY` | 108 | File extent data reference |
| `BTRFS_ROOT_ITEM_KEY` | 132 | Subvolume/snapshot root definition |
| `BTRFS_EXTENT_ITEM_KEY` | 168 | Extent allocation record |
| `BTRFS_BLOCK_GROUP_ITEM_KEY` | 192 | Block group descriptor |
| `BTRFS_CHUNK_ITEM_KEY` | 228 | Logical-to-physical chunk mapping |
| `BTRFS_DEV_ITEM_KEY` | 216 | Physical device descriptor |
| `BTRFS_FREE_SPACE_INFO_KEY` | 198 | Free space tree anchor |
| `BTRFS_FREE_SPACE_EXTENT_KEY` | 199 | Free space extent record |
| `BTRFS_FREE_SPACE_BITMAP_KEY` | 200 | Free space bitmap record |

The `offset` provides contextual differentiation for items sharing the same `objectid` and `type`. For a file extent data item, the offset represents the logical byte offset within the file where the data begins. For directory items, it often stores a CRC32C hash of the filename to facilitate rapid binary-search lookups within large directories.

**Key comparison algorithm:**
```
1. Compare objectid first
2. If equal, compare type
3. If equal, compare offset
```

### 2.2 Time Representation (`btrfs_timespec`)

Btrfs utilizes a custom structure for Unix time, designed specifically to overcome the Year 2038 problem inherent in legacy 32-bit Unix timestamps, while simultaneously providing nanosecond granularity.

The structure is exactly 12 bytes long:

| Offset | Size (Bytes) | Type | Description |
|--------|-------------|------|-------------|
| `0x00` | 8 | SINT | Number of seconds since the Unix epoch (1970-01-01T00:00:00Z). |
| `0x08` | 4 | UINT | Number of nanoseconds since the beginning of the second. |

The operating system driver must convert its native system time representation into this 12-byte format before committing any metadata changes (such as access, modification, or creation times) to the extent buffers.

---

## 3. Superblock Specification

The superblock is the primary entry point and the ultimate source of truth for the file system state. Multiple copies of the superblock are stored at fixed physical offsets on the underlying block devices to ensure resilient recovery in the event of localized media corruption.

### Superblock Mirror Locations

| Copy | Physical Byte Offset | Condition |
|------|---------------------|-----------|
| Primary | 64 KiB (`0x10000`) | Always present |
| Mirror 1 | 64 MiB (`0x4000000`) | If device ≥ 64 MiB |
| Mirror 2 | 256 GiB (`0x4000000000`) | If device ≥ 256 GiB |
| Mirror 3 | 1 PiB (`0x4000000000000`) | If device ≥ 1 PiB |

### Critical Superblock Fields

| Offset | Size (Bytes) | Description |
|--------|-------------|-------------|
| `0x40` | 8 | File system magic number (`_BHRfS_M` = `0x4D5F53665248425F`). |
| `0x48` | 8 | The `generation` (Transaction ID) of the file system. |
| `0x50` | 8 | Logical address pointing to the root node of the Root Tree. |
| `0x58` | 8 | Logical address pointing to the root node of the Chunk Tree. |
| `0x60` | 8 | Logical address pointing to the root node of the Log Tree. |
| `0xB4` | 8 | `compat_ro_flags`. |
| `0xBC` | 8 | `incompat_flags`. |
| `0xC4` | 2 | `csum_type`. Identifies the hash algorithm (see §9). |
| `0xC6` | 1 | `root_level`. The depth of the root tree. |

> [!WARNING]
> The `compat_ro_flags` indicate features that are backward-compatible for read-only mounts. If the custom OS driver does not support a feature flagged here, it may safely mount the file system in read-only mode, but it must actively **refuse to write** to prevent corruption. The `incompat_flags` indicate non-backward-compatible features. If the custom OS encounters an unrecognized flag in this field, it **must refuse to mount** the file system entirely.

The superblock also contains backup root slots, allowing the file system to recover by rolling back to an older, consistent state if the primary tree roots are unreadable.

---

## 4. B-Tree Mechanics and Node Memory Layout

Every data and metadata structure in Btrfs, with the singular exception of the superblock itself, resides within a B-tree. A Btrfs file system is conceptually a vast collection of interconnecting B-trees.

### Primary Tree Types

| Tree | Object ID | Description |
|------|-----------|-------------|
| Root Tree | 1 | Directory holding references to all other trees |
| Extent Tree | 2 | Manages raw space allocation and block reference counting |
| Chunk Tree | 3 | Manages logical-to-physical address translation |
| Dev Tree | 4 | Tracks per-device allocation |
| FS Tree | 5+ | Contains actual user files, directories, and subvolumes |
| Csum Tree | 7 | Houses cryptographic hashes of all data blocks |
| UUID Tree | 9 | Maps subvolume UUIDs to subvolume IDs |
| Free Space Tree | 10 | Tracks free space within block groups (Space Cache v2) |

### 4.1 The Btrfs Node Header (`btrfs_header`)

Regardless of whether a specific node is an internal node or a leaf node, it invariably begins with a standard, rigid **101-byte** `btrfs_header`:

| Offset | Size (Bytes) | Type | Description |
|--------|-------------|------|-------------|
| `0x00` | 32 | CSUM | Checksum of the node's contents, computed from offset `0x20` to end of block. |
| `0x20` | 16 | UUID | The File System UUID (`fsid`). Must match the superblock. |
| `0x30` | 8 | UINT | The logical address of this specific node (`bytenr`). |
| `0x38` | 7 | FIELD | Node flags (e.g., indicating written status). |
| `0x3F` | 1 | UINT | Backref revision. Typically 1 for modern implementations. |
| `0x40` | 16 | UUID | The UUID of the Chunk Tree. |
| `0x50` | 8 | UINT | `generation` (Transaction ID) expected for this block. |
| `0x58` | 8 | UINT | Owner ID. The Object ID of the tree that claims ownership. |
| `0x60` | 4 | UINT | Number of items (`nritems`) contained within the node. |
| `0x64` | 1 | UINT | `level` of the node. **0 indicates a leaf node.** |

> [!CAUTION]
> The OS **must** calculate the checksum of the incoming memory block and compare it against the CSUM field. The checksum only covers data from offset `0x20` onward (the first 32 bytes are excluded). The OS must also strictly verify that `bytenr` matches the exact logical address from which the node was read. If `generation` is older than expected by the parent pointer, the block **must** be classified as corrupted (phantom write detection).

### 4.2 Internal Node Layout (`level > 0`)

Internal nodes exclusively contain routing information. Immediately following the 101-byte header (at offset `0x65`), the internal node contains a flat, contiguous array of Key-Pointer pairs.

Each Key-Pointer pair is exactly **33 bytes**:

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `key` | 17 | A `btrfs_disk_key` representing the lowest key value in the child node. |
| `blockptr` | 8 | The logical address of the child node. |
| `generation` | 8 | The transaction generation of the child node. |

The OS must utilize an efficient **binary search** algorithm across this sorted array to rapidly traverse down to the target leaf node.

### 4.3 Leaf Node Layout (`level == 0`)

Leaf nodes store the actual file system structures. Btrfs leaf nodes employ an innovative **dual-growth memory layout** designed to eliminate internal fragmentation and maximize metadata density.

```
+------------------+----------------------------------------+-------------------+
| btrfs_header     | item[0] item[1] ... item[n] →     ← data[n] ... data[0] |
| (101 bytes)      | (forward-growing)                 (backward-growing)     |
+------------------+----------------------------------------+-------------------+
```

The `btrfs_item` structure is exactly **25 bytes** long:

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `key` | 17 | The `btrfs_disk_key` indexing the specific item. |
| `offset` | 4 | Byte offset of payload data, **relative to end of the header**. |
| `size` | 4 | Length of the payload data in bytes. |

> [!IMPORTANT]
> The `offset` field is calculated relative to the **end of the header** (byte 101), not the absolute beginning of the node. Payload data is packed starting from the absolute end of the block and growing **backward** toward the center. The leaf is full when the forward-growing item array collides with the backward-growing data region.

### 4.4 B-Tree Insertion and Deletion Algorithms

**Insertion:** If the target leaf node is full (items collide with payloads), the OS must execute a **node split** operation, dividing the full node into two separate nodes, redistributing keys and payloads evenly, and pushing a new routing key up to the parent internal node.

**Deletion:** When a key is deleted, the OS must ensure the node does not become too empty. If deletion causes underflow, the algorithm must attempt to **borrow** adjacent keys from sibling nodes. If siblings are also at minimum capacity, the OS must execute a **node merge** operation, combining the underflowing node with its sibling and recursively pulling the routing key down from the parent.

---

## 5. Core Item Specifications and Parsing

### 5.1 Root Item (`btrfs_root_item`)

The `btrfs_root_item` defines the internal state and entry point for an entire internal B-tree. These items reside exclusively within the Root Tree and represent the root directories of individual subvolumes and snapshots.

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `inode` | 160 | An embedded `btrfs_inode_item` representing the root directory. |
| `generation` | 8 | Transaction ID when the root item was created. |
| `root_dirid` | 8 | `BTRFS_FIRST_FREE_OBJECTID` (256) for user trees; 0 for metadata trees. |
| `bytenr` | 8 | Absolute logical disk offset of the root node of this tree. |
| `byte_limit` | 8 | Reserved (always 0). |
| `bytes_used` | 8 | Reserved. |
| `last_snapshot` | 8 | Transaction ID of the last successful snapshot of this tree. |
| `flags` | 8 | State flags (e.g., `BTRFS_ROOT_SUBVOL_RDONLY` for read-only snapshots). |
| `refs` | 4 | Global reference count for the root. |

When mounting a specific subvolume, the OS must query the Root Tree for the `btrfs_root_item` whose Object ID matches the requested subvolume ID, extract the `bytenr` field, and use it to read the top-level node of that subvolume.

### 5.2 Inode Item (`btrfs_inode_item`)

Btrfs allocates inodes dynamically—an inode only exists as a `btrfs_inode_item` inserted into the B-tree at the exact moment a file is created. This allows for a virtually unlimited number of files, bound only by available metadata space.

> [!NOTE]
> Standard Unix tools like `df -i` report zero available inodes on Btrfs volumes because inodes are dynamic. The OS must account for this when building user-facing utilities.

The `btrfs_inode_item` is exactly **160 bytes**:

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `generation` | 8 | Epoch when the inode was created or last modified. |
| `transid` | 8 | Transaction ID of the last modification. |
| `size` | 8 | Total logical size of the file in bytes. |
| `blocks` | 8 | Actual physical disk space consumed. |
| `block_group` | 8 | Block group identifier. |
| `nlink` | 4 | Number of hard links pointing to this inode. |
| `uid` | 4 | POSIX User identifier. |
| `gid` | 4 | POSIX Group identifier. |
| `mode` | 4 | POSIX access permissions and file type indicator. |
| `rdev` | 8 | Device identifier for special block/character files. |
| `flags` | 8 | Btrfs-specific flags (e.g., `NODATACOW`, `NODATASUM`). |
| `sequence` | 8 | Sequence counter for NFS compatibility. |
| `atime` | 12 | Access time (`btrfs_timespec`). |
| `ctime` | 12 | Change time (`btrfs_timespec`). |
| `mtime` | 12 | Modification time (`btrfs_timespec`). |
| `otime` | 12 | Creation time (`btrfs_timespec`). |

> [!WARNING]
> **Non-POSIX OS integration challenge:** When porting Btrfs to non-POSIX operating systems (such as Windows), the `uid` and `gid` fields have no direct equivalent. The custom OS must implement a robust identity mapping layer to translate Unix UIDs to Windows Security Identifiers (SIDs). POSIX ACLs must be translated into the native permission model, utilizing the `security.NTACL` extended attribute to persist them.

---

## 6. Block Group Allocation and Free Space Management

Btrfs divides available physical space into logical regions called "chunks" or "block groups":

| Type | Typical Size | Purpose |
|------|-------------|---------|
| Data | 1 GiB | User file content |
| Metadata | 256 MiB | Internal B-tree nodes and items |
| System | 32 MiB | Chunk tree and superblock data |

> [!CAUTION]
> Btrfs is highly susceptible to a specific failure state where it over-allocates raw disk space to Data block groups, leaving insufficient space for Metadata block groups. This triggers immediate `ENOSPC` and forces read-only mode even if gigabytes exist in Data groups. The OS kernel must implement dynamic reclaim thresholds and run periodic balancing operations in a background thread.

### 6.1 The Free Space Tree (Space Cache v2)

The Free Space Tree (`BTRFS_FREE_SPACE_TREE_OBJECTID` = 10) supersedes the legacy Space Cache v1. The custom OS should default entirely to `space_cache=v2`.

It records free space using two item types, dynamically switching based on fragmentation:

| Item Type | Description |
|-----------|-------------|
| `BTRFS_FREE_SPACE_INFO_KEY` | Anchor header for a block group; stores `extent_count` and `flags`. |
| `BTRFS_FREE_SPACE_EXTENT_KEY` | Tracks contiguous free regions; key offset = size of free space. |
| `BTRFS_FREE_SPACE_BITMAP_KEY` | Raw bitmap indicating block availability (for heavily fragmented groups). |

When a block group is largely empty, extent keys are used. When heavily fragmented, the tracking converts to a bitmap (`BTRFS_FREE_SPACE_USING_BITMAPS`). The OS must seamlessly support both representations and handle the dynamic transition between them.

---

## 7. Logical-to-Physical Mapping and RAID Topologies

Within the core file system code, Btrfs issues all read and write commands strictly to **logical addresses**. It is the responsibility of the OS's lower storage layer to parse the Chunk Tree and translate these logical addresses into physical addresses on specific hardware devices.

### 7.1 The `btrfs_chunk` Structure

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `length` | 8 | Total logical size of the chunk. |
| `stripe_len` | 8 | Size of an individual data stripe, fixed at **64 KiB** (`BTRFS_STRIPE_LEN`). |
| `type` | 8 | Bitmask: allocation type (Data/Metadata/System) + redundancy profile. |
| `num_stripes` | 2 | Number of physical device stripes in this chunk. |
| `sub_stripes` | 2 | Multiplier for RAID10 calculations. |

Following this header is an array of `btrfs_stripe` items:

| Field | Size (Bytes) | Description |
|-------|-------------|-------------|
| `devid` | 8 | Unique internal identifier of the physical block device. |
| `offset` | 8 | Starting physical byte offset on that device. |
| `dev_uuid` | 16 | UUID of the device (ensures integrity across device path changes). |

### 7.2 Address Translation Algorithms

When the OS receives a request to read a logical address, it must traverse the Chunk Tree for the matching `btrfs_chunk`:

```
relative_offset = target_logical_address - chunk_logical_base_address
```

Translation depends on the chunk's RAID profile:

**Single Profile:**
```
physical_address = stripe.offset + relative_offset
```

**RAID0 (Striping):**
```
stripe_index    = (relative_offset / stripe_len) % num_stripes
stripe_offset   = (relative_offset / (stripe_len * num_stripes)) * stripe_len
                  + (relative_offset % stripe_len)
physical_address = stripes[stripe_index].offset + stripe_offset
```

**RAID1 (Mirroring):**
`num_stripes = 2`. Physical address mirrors the Single calculation, but the OS must write to **both** stripes and may read from either. If a read fails checksum validation, the OS must automatically redirect to the alternate stripe (transparent self-healing).

**RAID10 (Striped Mirrors):**
Striping math identical to RAID0, but `sub_stripes = 2` identifies the paired mirror for each striped block. Minimum 4 devices required.

> [!WARNING]
> Btrfs RAID5 and RAID6 implementations are historically classified as **unstable** by Linux kernel developers, particularly regarding the "write hole" anomaly during power loss. Integration into a custom OS should be treated as highly experimental.

---

## 8. Data Integrity and Cryptographic Checksumming

Both metadata and user data are continuously protected by checksums. A read operation must **not** be returned to user-space until the checksum has been computed and verified.

### 8.1 Algorithm Selection

The `csum_type` field in the superblock dictates the hash algorithm:

| `csum_type` | Algorithm | Digest Width | ~Cycles / 4 KiB | Ratio vs CRC32C |
|-------------|-----------|-------------|-----------------|-----------------|
| 0 | CRC32C | 32-bit | 470 | 1.00× |
| 1 | xxHash | 64-bit | 870 | 1.90× |
| 2 | SHA256 | 256-bit | 7,600 | 16.00× |
| 3 | BLAKE2b | 256-bit | 10,000+ | 21.00× |

> [!IMPORTANT]
> CRC32C uses little-endian byte ordering with an initialization seed of `-1` (`0xFFFFFFFF`). Implementing this exact seeding is mandatory for backward compatibility with existing Linux volumes. The OS should heavily utilize hardware acceleration (SSE4.2, AVX) wherever the CPU supports it.

### 8.2 Checksum Storage

Data checksums are aggregated in the dedicated **Checksum Tree** (`BTRFS_CSUM_TREE_OBJECTID`) using `btrfs_csum_item` records. The key offset of each item corresponds to the logical byte offset of the file extent it protects. A single item payload can store an array of contiguous checksums covering a massive extent.

### 8.3 Background Scrub Process

The OS must provide a background thread to execute the Btrfs "Scrub" operation:

1. Sequentially traverse the Extent Tree
2. Read every allocated data and metadata block
3. Compute hash and verify against the Checksum Tree
4. If corruption detected on RAID1/RAID10: fetch replicated block from alternate disk, verify its checksum, overwrite the corrupted block with the healthy copy
5. Update internal error counters without interrupting user operations

---

## 9. VFS Abstraction and Memory Management Constraints

### 9.1 Extent Buffer Management

A Btrfs node is commonly formatted to **16 KiB** (spanning multiple 4 KiB memory pages). The custom OS must implement a robust `extent_buffer` data structure and API:

- Allocate an array of physical memory pages
- Issue scatter-gather disk I/O to populate them
- Present a contiguous logical memory view to B-tree traversal algorithms
- Implement specialized accessor functions that safely read/write little-endian values across arbitrary page boundaries
- Use `btrfs_map_token` to optimize repeated accesses to the same page

> [!IMPORTANT]
> The OS **cannot** simply cast a standard C pointer over the buffer because the underlying physical memory is not guaranteed to be contiguous. Accessor macros like `btrfs_item_key_to_cpu` and `btrfs_set_token_32` must be meticulously implemented.

### 9.2 Locking and Concurrency Control

The `extent_buffer` implementation must include robust read/write semaphores. The OS must implement a strict **lock-coupling algorithm** (crabbing) during tree traversal:

1. Lock the parent node
2. Lock the child node
3. Verify the path
4. Release the parent lock only when the child is confirmed as the final target and no node splitting is required

> [!CAUTION]
> Failure to implement strict lock ordering will cause **catastrophic deadlocks** under heavy concurrent I/O workloads.

### 9.3 Delayed Allocation and ENOSPC Handling

When a user application executes a `write()` system call, the OS must **not** immediately allocate physical disk blocks. Instead:

1. Reserve logical space in memory
2. Copy user data into the page cache
3. On synchronization events (`fsync`, memory pressure flush):
   - Allocate contiguous physical blocks
   - Compress data (if zlib, LZO, or ZSTD compression is enabled)
   - Compute cryptographic checksums
   - Execute physical write operations

> [!CAUTION]
> Btrfs must accurately predict the amount of metadata required to complete a delayed write **before** the write is accepted. Failure to track this will result in catastrophic transaction aborts when the background flusher discovers it cannot write the cached data because the metadata trees are full. The OS must preemptively force physical allocation or return `ENOSPC` before accepting data into the cache.

---

## 10. Strategic Implementation Roadmap for Impossible OS

### Phase 1: Minimal Read-Only Architecture

1. **Superblock Parsing:** Read the primary superblock at offset `0x10000`. Validate the magic number (`_BHRfS_M`). Parse `incompat_flags` — reject mount if unsupported flags are present.
2. **Chunk Tree Bootstrap:** Parse the superblock's inline chunk map to bootstrap logical-to-physical address translation.
3. **Root Tree Traversal:** Read the Root Tree root node. Locate the FS Tree root item for the default subvolume (ID 5 or as specified at mount time).
4. **Inode Lookup:** Traverse the FS Tree to locate the root directory inode. Parse `btrfs_inode_item` for attributes.
5. **Directory Reading:** Parse `DIR_ITEM` and `DIR_INDEX` keys to enumerate directory entries.
6. **File Data Reading:** Parse `EXTENT_DATA` keys, resolve logical-to-physical mapping via the Chunk Tree, read and return data blocks after checksum verification.

### Phase 2: Write Support and Transactions

1. Implement the full COW write path with path-copying
2. Implement the transaction manager and generation counter
3. Implement extent allocation from the Extent Tree
4. Implement reference counting for shared blocks
5. Implement delayed allocation with ENOSPC tracking
6. Test with QEMU using `virtio-blk` backing a Btrfs-formatted disk image

### Phase 3: Advanced Features

1. Subvolume creation and management
2. Writable snapshot support
3. Transparent compression (ZSTD preferred)
4. Background scrub and self-healing
5. RAID1 mirror management
6. Free Space Tree (v2) support
