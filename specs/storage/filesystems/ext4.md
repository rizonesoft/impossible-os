# Specification and Implementation Architecture of the Fourth Extended Filesystem (ext4)

## 1. Architectural Overview and Fundamental Design Principles

The Fourth Extended Filesystem (ext4) represents a foundational pillar of modern Linux storage architecture, evolving from its predecessors, ext2 and ext3, to address the profound scalability limitations inherent in early Unix-like filesystems. Designed to support massive storage arrays and high-performance throughput, ext4 introduces sweeping architectural changes while maintaining a high degree of backward and forward compatibility. For operating system developers implementing custom kernel drivers or bootloaders, understanding the exact on-disk binary layout, data structures, and feature flags of ext4 is a strict prerequisite for ensuring data integrity and interoperability.

The evolution from the ext3 architecture to ext4 is primarily characterized by the transition from indirect block mapping to extent-based allocation, the expansion from 32-bit to 64-bit block addressing, and the implementation of delayed allocation and multiblock allocation algorithms. These enhancements allow the ext4 filesystem to theoretically support maximum volume sizes of up to 1 Exbibyte (EiB) and maximum individual file sizes ranging from 16 to 256 Tebibytes (TiB), depending entirely on the configured block size (which typically ranges from 4 KiB to 64 KiB). Furthermore, ext4 incorporates a highly resilient journaling layer known as the Journaling Block Device 2 (JBD2), which guarantees metadata consistency across system crashes, kernel panics, and unexpected power loss events.

> [!IMPORTANT]
> All ext4 on-disk data structures, with the strict exception of the JBD2 journal, are recorded in **little-endian** byte order. Conversely, the JBD2 subsystem writes all of its internal structures in **big-endian** byte order. This architectural dichotomy requires careful byte-swapping implementation in custom filesystem drivers that operate on big-endian hardware architectures.

---

## 2. Global Storage Topology and Partition Formatting

To implement a functional ext4 driver, the storage volume must be viewed as a contiguous array of logical blocks, which are subsequently clustered into larger administrative domains known as **Block Groups**. The size of a block is determined at formatting time by the `mke2fs` utility and is typically 4096 bytes (4 KiB) for modern media, though 1024-byte and 2048-byte sizes are utilized in constrained environments.

### Boot Padding and Superblock Location

The first 1024 bytes of the storage volume are perpetually reserved and left entirely unused by the ext4 filesystem. This space is historically allocated for the installation of master boot records, boot sectors (such as GRUB or LILO), partition tables, and other hardware-specific oddities that expect to reside at the absolute beginning of a physical disk.

The **Superblock**, the paramount metadata structure of the volume, is invariably located at an absolute byte offset of **1024** from the start of the partition. The block number of the Superblock varies depending on the hardware block size:

- On filesystems utilizing a **1024-byte block size**, the 1024-byte boot padding consumes block 0 entirely, meaning the Superblock occupies **block 1**.
- For all other filesystems (e.g., the standard **4096-byte block size**), the 1024-byte padding occupies the first quarter of block 0, and the Superblock immediately follows within the same block, leaving the remaining 2048 bytes of block 0 unused.

### Block Group Geometry

The total capacity of a single Block Group is explicitly defined by the Superblock's `s_blocks_per_group` field, which conventionally equates to eight times the block size in bytes. Consequently:

- A filesystem utilizing **4 KiB blocks** will consistently contain **32,768 blocks** per Block Group.
- Each Block Group spans exactly **128 Mebibytes (MiB)**.
- The total number of Block Groups on the disk is derived by dividing the total size of the filesystem by the size of a single Block Group.

### Sparse Superblock Backups

Because the Superblock and the Group Descriptor Table (GDT) are overwhelmingly critical to filesystem integrity, the ext4 architecture dictates the maintenance of redundant backups. Under the modern `sparse_super` configuration (enabled by default), backup copies are exclusively written to:

- Block Group 0
- Any Block Group whose index is a mathematical power of **3**, **5**, or **7** (e.g., groups 3, 5, 7, 9, 25, 27, 49, 81, 125, 243, 343)

This sparse distribution ensures high survivability against localized physical platter damage while preserving storage space that would otherwise be consumed by thousands of redundant metadata copies on multi-terabyte drives.

---

## 3. The Superblock: Foundational Metadata and Volume Parameters

The Superblock (`struct ext4_super_block`) is a 1024-byte structure that acts as the absolute source of truth for the filesystem's global parameters and state. An operating system driver must successfully parse, validate, and interpret the primary Superblock before any directory traversal or read operations can commence.

### Magic Number Validation

Validation of the Superblock begins by verifying the magic signature, which is perpetually set to **`0xEF53`**. This 16-bit little-endian value is located at byte offset `0x38` (56 bytes) within the Superblock structure, which corresponds to an absolute physical byte offset of **1080** from the start of the partition. Because the value is little-endian, it is physically stored on the disk as the byte sequence `0x53 0xEF`.

> [!CAUTION]
> If a filesystem driver reads this offset and this magic number is absent or corrupted, the driver must instantly abort the mount procedure, as the volume is either fundamentally corrupt or formatted with an unrecognized filesystem type.

### Fundamental Superblock Fields

| Offset (Hex) | Offset (Dec) | Size | Field Name               | Description                                                              |
| ------------ | ------------ | ---- | ------------------------ | ------------------------------------------------------------------------ |
| `0x00`       | 0            | 4    | `s_inodes_count`         | Total number of inodes on the volume                                     |
| `0x04`       | 4            | 4    | `s_blocks_count_lo`      | Total block count (lower 32 bits)                                        |
| `0x08`       | 8            | 4    | `s_r_blocks_count_lo`    | Reserved blocks for superuser (lower 32 bits)                            |
| `0x0C`       | 12           | 4    | `s_free_blocks_count_lo` | Total unallocated blocks (lower 32 bits)                                 |
| `0x10`       | 16           | 4    | `s_free_inodes_count`    | Total unallocated inodes                                                 |
| `0x14`       | 20           | 4    | `s_first_data_block`     | Block number containing the Superblock (0 for 4K blocks, 1 for 1K)      |
| `0x18`       | 24           | 4    | `s_log_block_size`       | Block size = `1024 << s_log_block_size`                                  |
| `0x1C`       | 28           | 4    | `s_log_cluster_size`     | Cluster size = `1024 << s_log_cluster_size` (bigalloc)                   |
| `0x20`       | 32           | 4    | `s_blocks_per_group`     | Number of blocks per Block Group                                         |
| `0x24`       | 36           | 4    | `s_clusters_per_group`   | Clusters per group (bigalloc) or same as blocks_per_group                |
| `0x28`       | 40           | 4    | `s_inodes_per_group`     | Number of inodes per Block Group                                         |
| `0x2C`       | 44           | 4    | `s_mtime`                | Last mount time (POSIX timestamp)                                        |
| `0x30`       | 48           | 4    | `s_wtime`                | Last write time (POSIX timestamp)                                        |
| `0x34`       | 52           | 2    | `s_mnt_count`            | Mount count since last full `fsck`                                       |
| `0x36`       | 54           | 2    | `s_max_mnt_count`        | Maximum mount count before forced `fsck`                                 |
| `0x38`       | 56           | 2    | `s_magic`                | Magic signature: **`0xEF53`**                                            |
| `0x3A`       | 58           | 2    | `s_state`                | Filesystem state (1 = clean, 2 = errors, 4 = orphans)                    |
| `0x3C`       | 60           | 2    | `s_errors`               | Behavior on error (1 = continue, 2 = remount RO, 3 = panic)             |
| `0x3E`       | 62           | 2    | `s_minor_rev_level`      | Minor revision level                                                     |
| `0x40`       | 64           | 4    | `s_lastcheck`            | Time of last `fsck` (POSIX timestamp)                                    |
| `0x44`       | 68           | 4    | `s_checkinterval`        | Maximum time between `fsck` runs                                         |
| `0x48`       | 72           | 4    | `s_creator_os`           | Creator OS (0 = Linux, 3 = FreeBSD)                                      |
| `0x4C`       | 76           | 4    | `s_rev_level`            | Revision level (0 = original, 1 = dynamic inode sizes)                   |
| `0x50`       | 80           | 2    | `s_def_resuid`           | Default UID for reserved blocks                                          |
| `0x52`       | 82           | 2    | `s_def_resgid`           | Default GID for reserved blocks                                          |

### Extended Superblock Fields (Revision 1+)

| Offset (Hex) | Offset (Dec) | Size | Field Name               | Description                                              |
| ------------ | ------------ | ---- | ------------------------ | -------------------------------------------------------- |
| `0x54`       | 84           | 4    | `s_first_ino`            | First non-reserved inode (typically 11)                   |
| `0x58`       | 88           | 2    | `s_inode_size`           | Inode size in bytes (128 for ext2/ext3, 256 for ext4)     |
| `0x5A`       | 90           | 2    | `s_block_group_nr`       | Block group number of this Superblock (for backups)       |
| `0x5C`       | 92           | 4    | `s_feature_compat`       | Compatible feature bitmask                                |
| `0x60`       | 96           | 4    | `s_feature_incompat`     | Incompatible feature bitmask                              |
| `0x64`       | 100          | 4    | `s_feature_ro_compat`    | Read-only compatible feature bitmask                      |
| `0x68`       | 104          | 16   | `s_uuid`                 | 128-bit filesystem UUID                                   |
| `0x78`       | 120          | 16   | `s_volume_name`          | Volume label (null-terminated)                            |
| `0x88`       | 136          | 64   | `s_last_mounted`         | Last mount point path                                     |
| `0xC8`       | 200          | 4    | `s_algorithm_usage_bitmap` | Compression algorithm bitmap                            |
| `0xFE`       | 254          | 2    | `s_desc_size`            | Group descriptor size (32 or 64 bytes)                    |
| `0x150`      | 336          | 4    | `s_blocks_count_hi`      | Total block count (upper 32 bits, 64-bit mode)            |
| `0x154`      | 340          | 4    | `s_r_blocks_count_hi`    | Reserved blocks (upper 32 bits)                           |
| `0x158`      | 344          | 4    | `s_free_blocks_count_hi` | Free blocks (upper 32 bits)                               |
| `0x15C`      | 348          | 2    | `s_min_extra_isize`      | Minimum extra inode bytes required                        |
| `0x15E`      | 350          | 2    | `s_want_extra_isize`     | Desired extra inode bytes                                 |
| `0x160`      | 352          | 4    | `s_flags`                | Miscellaneous flags                                       |
| `0x174`      | 372          | 1    | `s_log_groups_per_flex`  | Flex group size = `2 ^ s_log_groups_per_flex`             |
| `0x175`      | 373          | 1    | `s_checksum_type`        | Metadata checksum algorithm (1 = CRC32C)                  |
| `0x178`      | 376          | 8    | `s_kbytes_written`       | KB written (lifetime)                                     |
| `0x270`      | 624          | 4    | `s_checksum_seed`        | CRC32C checksum seed (if `CSUM_SEED` feature set)         |
| `0x3FC`      | 1020         | 4    | `s_checksum`             | CRC32C checksum of the entire Superblock                  |

### Block Size Calculation

The filesystem does not store the block size as a raw integer. The hardware block size is algorithmically derived:

```c
block_size = 1024 << s_log_block_size;
```

| `s_log_block_size` | Block Size |
|--------------------|-----------|
| 0 | 1024 bytes (1 KiB) |
| 1 | 2048 bytes (2 KiB) |
| 2 | 4096 bytes (4 KiB) |
| 4 | 16384 bytes (16 KiB) |
| 6 | 65536 bytes (64 KiB) |

### Timestamp Extensions

To support timestamps beyond the year 2038 limitation of 32-bit integers, ext4 introduced 64-bit timestamp extensions. The high 8 bits of the write time, mount time, mkfs time, and last check time are stored sequentially at offsets `0x274`–`0x277` (bytes 628–631) in the Superblock.

---

## 4. Feature Flag Matrices and Operating System Compatibility

The ext4 specification relies heavily on feature flags to maintain backward compatibility while continuously introducing modern capabilities. These flags are divided into three distinct 32-bit bitmasks:

- **Compatible Features** (`s_feature_compat` at offset `0x5C`) -- May be ignored; filesystem still usable
- **Incompatible Features** (`s_feature_incompat` at offset `0x60`) -- **Must be supported or mount rejected**
- **Read-Only Compatible Features** (`s_feature_ro_compat` at offset `0x64`) -- Mount as read-only if unsupported

### Incompatible Feature Flags

> [!CAUTION]
> If a custom driver detects any set bit in `s_feature_incompat` that it does not explicitly support, the driver is **strictly forbidden** from mounting the filesystem. Attempting to interact with unsupported incompatible features will inevitably cause catastrophic data corruption.

| Feature Flag             | Hex Value  | Description                                                                         |
| ------------------------ | ---------- | ----------------------------------------------------------------------------------- |
| `INCOMPAT_FILETYPE`      | `0x0002`   | Directory entries contain a file type byte                                          |
| `INCOMPAT_RECOVER`       | `0x0004`   | Filesystem needs journal recovery                                                   |
| `INCOMPAT_JOURNAL_DEV`   | `0x0008`   | Volume is a dedicated external journal device                                       |
| `INCOMPAT_META_BG`       | `0x0010`   | Meta Block Group descriptor layout                                                  |
| `INCOMPAT_EXTENTS`       | `0x0040`   | Extent-based file data mapping. **Mandatory for modern ext4**                       |
| `INCOMPAT_64BIT`         | `0x0080`   | 64-bit block numbers                                                                |
| `INCOMPAT_MMP`           | `0x0100`   | Multiple Mount Protection                                                           |
| `INCOMPAT_FLEX_BG`       | `0x0200`   | Flexible Block Groups                                                               |
| `INCOMPAT_EA_INODE`      | `0x0400`   | Extended attributes stored in dedicated inodes                                      |
| `INCOMPAT_DIRDATA`       | `0x1000`   | Directory entries contain extra data                                                |
| `INCOMPAT_CSUM_SEED`     | `0x2000`   | Checksum seed in Superblock -- allows UUID changes without recalculating checksums   |
| `INCOMPAT_LARGEDIR`      | `0x4000`   | Directories support 3-level HTree depth (> 2 GiB directory sizes)                   |
| `INCOMPAT_INLINE_DATA`   | `0x8000`   | Small files store payload directly within the inode's `i_block` array               |
| `INCOMPAT_ENCRYPT`       | `0x10000`  | Filesystem contains encrypted inodes                                                |
| `INCOMPAT_CASEFOLD`      | `0x20000`  | Case-insensitive filename lookups                                                   |

### Read-Only Compatible Feature Flags

If unrecognized flags are present in `s_feature_ro_compat`, the OS may mount the filesystem but must enforce **read-only** mode:

| Feature Flag                | Hex Value | Description                                                      |
| --------------------------- | --------- | ---------------------------------------------------------------- |
| `RO_COMPAT_SPARSE_SUPER`   | `0x0001`  | Sparse superblock backups (groups 0, powers of 3/5/7)            |
| `RO_COMPAT_LARGE_FILE`     | `0x0002`  | Files larger than 2 GiB exist                                    |
| `RO_COMPAT_BTREE_DIR`      | `0x0004`  | (Unused)                                                         |
| `RO_COMPAT_HUGE_FILE`      | `0x0008`  | File sizes in units of filesystem blocks (not 512-byte sectors)  |
| `RO_COMPAT_GDT_CSUM`       | `0x0010`  | Group descriptor checksums (ext3-style, not CRC32C)              |
| `RO_COMPAT_DIR_NLINK`      | `0x0020`  | Directories with > 64,999 subdirectories                         |
| `RO_COMPAT_EXTRA_ISIZE`    | `0x0040`  | Inodes have extended fields (`i_extra_isize`)                    |
| `RO_COMPAT_HAS_SNAPSHOT`   | `0x0080`  | Snapshot support                                                 |
| `RO_COMPAT_QUOTA`          | `0x0100`  | Quota support                                                    |
| `RO_COMPAT_BIGALLOC`       | `0x0200`  | Bitmap tracks clusters instead of blocks                         |
| `RO_COMPAT_METADATA_CSUM`  | `0x0400`  | **Metadata checksumming (CRC32C)**                               |
| `RO_COMPAT_READONLY`       | `0x1000`  | Filesystem is read-only                                          |
| `RO_COMPAT_PROJECT`        | `0x2000`  | Project quota support                                            |
| `RO_COMPAT_VERITY`         | `0x8000`  | fs-verity support                                                |

### Compatible Feature Flags

| Feature Flag              | Hex Value | Description                                                       |
| ------------------------- | --------- | ----------------------------------------------------------------- |
| `COMPAT_DIR_PREALLOC`     | `0x0001`  | Directory preallocation                                           |
| `COMPAT_IMAGIC_INODES`    | `0x0002`  | AFS server inodes                                                 |
| `COMPAT_HAS_JOURNAL`      | `0x0004`  | **Filesystem has a JBD2 journal**                                 |
| `COMPAT_EXT_ATTR`         | `0x0008`  | Extended attributes supported                                     |
| `COMPAT_RESIZE_INODE`     | `0x0010`  | Online resize reserved GDT blocks                                 |
| `COMPAT_DIR_INDEX`        | `0x0020`  | HTree directory indexing                                          |
| `COMPAT_SPARSE_SUPER2`    | `0x0200`  | Only two superblock backups (at groups specified in Superblock)    |
| `COMPAT_FAST_COMMIT`      | `0x0400`  | JBD2 fast commits                                                 |
| `COMPAT_STABLE_INODES`    | `0x0800`  | Stable inode numbers (encryption)                                 |
| `COMPAT_ORPHAN_FILE`      | `0x1000`  | Orphan file for tracking orphaned inodes                          |

---

## 5. Block Group Descriptors and Advanced Grouping Dynamics

Immediately following the Superblock--and any associated boot padding--the filesystem stores the **Group Descriptor Table (GDT)**. The GDT is an array of `ext4_group_desc` structures, sequentially detailing the physical state and logical layout of every single Block Group.

### Group Descriptor Structure

In legacy ext2/ext3, the descriptor was a fixed 32 bytes. When `INCOMPAT_64BIT` is enabled, it expands to at least 64 bytes (stored in `s_desc_size`).

| Offset (Hex) | Size | Field Name | Description |
|-------------|------|------------|-------------|
| `0x00` | 4 | `bg_block_bitmap_lo` | Block bitmap location (lower 32 bits) |
| `0x04` | 4 | `bg_inode_bitmap_lo` | Inode bitmap location (lower 32 bits) |
| `0x08` | 4 | `bg_inode_table_lo` | Inode table location (lower 32 bits) |
| `0x0C` | 2 | `bg_free_blocks_count_lo` | Free blocks count (lower 16 bits) |
| `0x0E` | 2 | `bg_free_inodes_count_lo` | Free inodes count (lower 16 bits) |
| `0x10` | 2 | `bg_used_dirs_count_lo` | Directory count (lower 16 bits) |
| `0x12` | 2 | `bg_flags` | Block group flags |
| `0x14` | 4 | `bg_exclude_bitmap_lo` | Exclude bitmap (snapshots) |
| `0x18` | 2 | `bg_block_bitmap_csum_lo` | Block bitmap checksum (lower 16 bits) |
| `0x1A` | 2 | `bg_inode_bitmap_csum_lo` | Inode bitmap checksum (lower 16 bits) |
| `0x1C` | 2 | `bg_itable_unused_lo` | Unused inode count (lower 16 bits) |
| `0x1E` | 2 | `bg_checksum` | Group descriptor checksum (CRC16 or lower 16 of CRC32C) |

**64-bit extension fields (offset `0x20`+):**

| Offset (Hex) | Size | Field Name | Description |
|-------------|------|------------|-------------|
| `0x20` | 4 | `bg_block_bitmap_hi` | Block bitmap location (upper 32 bits) |
| `0x24` | 4 | `bg_inode_bitmap_hi` | Inode bitmap location (upper 32 bits) |
| `0x28` | 4 | `bg_inode_table_hi` | Inode table location (upper 32 bits) |
| `0x2C` | 2 | `bg_free_blocks_count_hi` | Free blocks count (upper 16 bits) |
| `0x2E` | 2 | `bg_free_inodes_count_hi` | Free inodes count (upper 16 bits) |
| `0x30` | 2 | `bg_used_dirs_count_hi` | Directory count (upper 16 bits) |
| `0x32` | 2 | `bg_itable_unused_hi` | Unused inode count (upper 16 bits) |
| `0x34` | 4 | `bg_exclude_bitmap_hi` | Exclude bitmap (upper 32 bits) |
| `0x38` | 2 | `bg_block_bitmap_csum_hi` | Block bitmap checksum (upper 16 bits) |
| `0x3A` | 2 | `bg_inode_bitmap_csum_hi` | Inode bitmap checksum (upper 16 bits) |

A custom OS driver must seamlessly concatenate the `_lo` and `_hi` halves to form valid 64-bit storage pointers.

### Flexible Block Group Mechanism (flex_bg)

When `INCOMPAT_FLEX_BG` is enabled, the filesystem aggregates the metadata structures--Block Bitmaps, Inode Bitmaps, and Inode Tables--from multiple sequential Block Groups and stores them contiguously in the first Block Group of that "flex sequence".

The number of block groups clustered into a single flex group is determined by:

```c
flex_group_size = 1 << s_log_groups_per_flex;  /* s_log_groups_per_flex at Superblock offset 0x174 */
```

This architecture concentrates metadata into a continuous disk span, vastly accelerating operations that require intensive metadata traversal, while leaving the remaining Block Groups in the flex cluster to exclusively host contiguous data blocks.

### Meta Block Group Mechanism (meta_bg)

The `meta_bg` feature resolves a fundamental GDT scalability limitation. In standard ext2/ext3, all Group Descriptors must reside sequentially in a massive table immediately following the Superblock in Block Group 0. Given a default 128 MiB block group size and an expanding 64-byte descriptor size, the total filesystem size was bottlenecked at ~256 TiB.

With `meta_bg`, Block Groups are clustered into Meta Block Groups. For a standard 4 KiB block filesystem, a Meta Block Group consists of **64 standard Block Groups**. Rather than one global table, descriptors for these 64 groups are stored within the first block group of the meta-cluster itself. Backup copies are stored in the second and last block groups. This enables the maximum number of Block Groups to scale to 2^32, supporting **512 PiB to 1 EiB** filesystem maximums.

### Allocation Bitmaps and Bigalloc

The **Block Bitmap** and **Inode Bitmap** are discrete, single-block structures used by the multiblock allocator (`mballoc`) to track resource availability. Each bit corresponds to a single block (or cluster if `bigalloc` is enabled); `1` = allocated, `0` = free.

When `bigalloc` is activated, each bitmap bit represents a "cluster" of blocks (a power of two, such as 16 or 64 blocks). This reduces bitmap size and optimizes contiguous allocation for massive files.

---

## 6. The Inode Table and Metadata Encapsulation

The inode (`struct ext4_inode`) is the core metadata encapsulation for any filesystem object--regular files, directories, symbolic links, named pipes, sockets, and device nodes.

### Locating Inodes Algorithmically

Because ext4 dictates that there is no inode 0, calculations use a base-1 index:

```c
block_group    = (inode_number - 1) / s_inodes_per_group;
local_index    = (inode_number - 1) % s_inodes_per_group;
byte_offset    = local_index * s_inode_size;
inode_table_block = bg_inode_table_lo | ((uint64_t)bg_inode_table_hi << 32);
```

### Inode Structure (Core 128 Bytes)

| Offset (Hex) | Size | Field Name | Description |
|-------------|------|------------|-------------|
| `0x00` | 2 | `i_mode` | File mode and type (permissions + file type in upper 4 bits) |
| `0x02` | 2 | `i_uid` | Owner UID (lower 16 bits) |
| `0x04` | 4 | `i_size_lo` | File size in bytes (lower 32 bits) |
| `0x08` | 4 | `i_atime` | Last access time (POSIX seconds) |
| `0x0C` | 4 | `i_ctime` | Inode change time (POSIX seconds) |
| `0x10` | 4 | `i_mtime` | Last modification time (POSIX seconds) |
| `0x14` | 4 | `i_dtime` | Deletion time |
| `0x18` | 2 | `i_gid` | Group ID (lower 16 bits) |
| `0x1A` | 2 | `i_links_count` | Hard link count |
| `0x1C` | 4 | `i_blocks_lo` | Block count (lower 32 bits, in 512-byte units unless `HUGE_FILE`) |
| `0x20` | 4 | `i_flags` | Inode flags (extent, inline, immutable, etc.) |
| `0x24` | 4 | `i_osd1` | OS-dependent value 1 (Linux: high 32-bit version) |
| `0x28` | 60 | `i_block[15]` | **Block map / Extent tree root / Inline data** |
| `0x64` | 4 | `i_generation` | File version for NFS |
| `0x68` | 4 | `i_file_acl_lo` | Extended attribute block (lower 32 bits) |
| `0x6C` | 4 | `i_size_high` | File size (upper 32 bits, for regular files) |
| `0x70` | 4 | `i_obso_faddr` | Obsolete fragment address |
| `0x74` | 12 | `i_osd2` | OS-dependent value 2 |

### Extended Inode Fields (256-byte inode, offset `0x80`+)

| Offset (Hex) | Size | Field Name | Description |
|-------------|------|------------|-------------|
| `0x80` | 2 | `i_extra_isize` | Size of extra inode fields beyond 128 bytes |
| `0x82` | 2 | `i_checksum_hi` | Inode checksum (upper 16 bits) |
| `0x84` | 4 | `i_ctime_extra` | Change time extra (nanoseconds << 2 | epoch bits) |
| `0x88` | 4 | `i_mtime_extra` | Modification time extra |
| `0x8C` | 4 | `i_atime_extra` | Access time extra |
| `0x90` | 4 | `i_crtime` | File creation time (POSIX seconds) |
| `0x94` | 4 | `i_crtime_extra` | Creation time extra (nanoseconds) |
| `0x98` | 4 | `i_version_hi` | 64-bit version (upper 32 bits) |
| `0x9C` | 4 | `i_projid` | Project ID |

### Inode Flags (`i_flags`)

| Flag | Hex Value | Description |
|------|-----------|-------------|
| `EXT4_SECRM_FL` | `0x00000001` | Secure deletion |
| `EXT4_UNRM_FL` | `0x00000002` | Record for undelete |
| `EXT4_COMPR_FL` | `0x00000004` | Compressed file |
| `EXT4_SYNC_FL` | `0x00000008` | Synchronous updates |
| `EXT4_IMMUTABLE_FL` | `0x00000010` | Immutable file |
| `EXT4_APPEND_FL` | `0x00000020` | Append only |
| `EXT4_NODUMP_FL` | `0x00000040` | Do not dump/backup |
| `EXT4_NOATIME_FL` | `0x00000080` | Do not update atime |
| `EXT4_INDEX_FL` | `0x00001000` | HTree-indexed directory |
| `EXT4_JOURNAL_DATA_FL` | `0x00004000` | File data journaled |
| `EXT4_DIRSYNC_FL` | `0x00010000` | Directory synchronous updates |
| `EXT4_TOPDIR_FL` | `0x00020000` | Top of directory hierarchy |
| `EXT4_HUGE_FILE_FL` | `0x00040000` | Huge file (blocks in FS units, not 512-byte) |
| `EXT4_EXTENTS_FL` | `0x00080000` | **Inode uses extent tree (not indirect blocks)** |
| `EXT4_VERITY_FL` | `0x00100000` | fs-verity protected |
| `EXT4_EA_INODE_FL` | `0x00200000` | Inode stores extended attribute value |
| `EXT4_INLINE_DATA_FL` | `0x10000000` | **Inode has inline data in `i_block`** |
| `EXT4_CASEFOLD_FL` | `0x40000000` | Case-insensitive directory |

### The `i_block` Array (60 Bytes at Offset `0x28`)

The functionality of this 60-byte region is determined dynamically by inode flags:

- **`EXT4_EXTENTS_FL` set** → Extent tree root node (most common, see §7)
- **`EXT4_INLINE_DATA_FL` set** → Small file payload stored directly in these 60 bytes
- **Neither set** → Legacy indirect block map (15 × 4-byte pointers, see §12)

### Special Inode Reservations

| Inode | Purpose |
|-------|---------|
| 1 | Defective block list |
| 2 | **Root directory (`/`)** -- entry point for all path resolution |
| 3 | User quota file |
| 4 | Group quota file |
| 5 | Boot loader inode |
| 6 | Undelete directory |
| 7 | Reserved group descriptors inode |
| 8 | **JBD2 journal file** |
| 9 | Exclude inode (snapshots) |
| 10 | Replica inode |
| 11 | First non-reserved inode (traditionally `lost+found`) |

---

## 7. Extent-Based Data Allocation and Tree Traversals

The introduction of extent-based mapping is ext4's most significant leap in managing massive files with low overhead. An **extent** is a continuous, uninterrupted run of physical blocks.

### Extent Tree Architecture

Extents are organized as a B-tree. Every node begins with an **Extent Header** (`struct ext4_extent_header`):

```c
struct ext4_extent_header {
    uint16_t eh_magic;    /* Magic: 0xF30A */
    uint16_t eh_entries;  /* Number of valid entries */
    uint16_t eh_max;      /* Maximum entries capacity */
    uint16_t eh_depth;    /* 0 = leaf node (extents), >0 = internal node (indices) */
    uint32_t eh_generation; /* Generation (for checksumming) */
};
```

### Extent Index Node (`eh_depth > 0`)

Each 12-byte `ext4_extent_idx` entry maps a logical block range to a child tree node:

```c
struct ext4_extent_idx {
    uint32_t ei_block;     /* Logical block covered by this index */
    uint32_t ei_leaf_lo;   /* Physical block of child node (lower 32 bits) */
    uint16_t ei_leaf_hi;   /* Physical block (upper 16 bits) */
    uint16_t ei_unused;    /* Reserved */
};
```

### Extent Leaf Node (`eh_depth == 0`)

Each 12-byte `ext4_extent` entry directly maps logical blocks to physical blocks:

```c
struct ext4_extent {
    uint32_t ee_block;     /* First logical block this extent covers */
    uint16_t ee_len;       /* Number of blocks (max 32768 = 128 MiB at 4K blocks) */
    uint16_t ee_start_hi;  /* Physical starting block (upper 16 bits) */
    uint32_t ee_start_lo;  /* Physical starting block (lower 32 bits) */
};
```

> [!NOTE]
> If bit 15 of `ee_len` is set, the extent is uninitialized (preallocated but unwritten). The actual length is `ee_len - 32768`. Reads from uninitialized extents return zeroes.

### Tree Capacity

| Location | Size | Max Entries |
|----------|------|-------------|
| Inode `i_block` (root) | 60 bytes = 12-byte header + 48 bytes | 4 extents or 4 indices |
| 4 KiB data block | 4096 bytes = 12-byte header + 4084 bytes | 340 entries |

### Traversal Algorithm

```
1. Read extent header from inode i_block[0..11]
2. Verify eh_magic == 0xF30A
3. If eh_depth == 0:
     → Leaf node: binary search eh_entries extents for target logical block
     → Return physical block = ee_start + (logical_block - ee_block)
4. If eh_depth > 0:
     → Index node: binary search for largest ei_block <= target logical block
     → Read child block at ei_leaf physical address
     → Recurse from step 2
```

### Extent Tail Checksum

The final 4 bytes of an extent tree block (either in-inode or in a separate block) contain the `ext4_extent_tail` with a CRC32C checksum:

```c
struct ext4_extent_tail {
    uint32_t et_checksum;  /* CRC32C of extent block header + entries */
};
```

### Implementation Optimization: Extent Caching

For highly fragmented files resulting in deep extent trees, iteratively reading index blocks to resolve every logical block introduces severe I/O latency. A high-performance driver should traverse the extent tree **once** upon opening the file, extracting all leaf extents into a sorted in-memory cache. This allows instantaneous logical-to-physical address resolution.

---

## 8. Directory Indexing: From Linear Entries to HTree Hashes

In ext4, a directory is technically a standard file--with its own inode and extent tree--whose data blocks contain a map linking filenames to inode numbers.

### Linear Directory Entries (`ext4_dir_entry_2`)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 4 | `inode` | Target inode number (0 = unused/deleted) |
| `0x04` | 2 | `rec_len` | Total length of this entry (offset to next entry) |
| `0x06` | 1 | `name_len` | Actual character count of the filename |
| `0x07` | 1 | `file_type` | File type code (see table below) |
| `0x08` | var | `name` | Filename string (not null-terminated) |

Entries are aligned to 4-byte boundaries. The final entry's `rec_len` extends to the end of the block.

### File Type Codes

| Code | Type |
|------|------|
| 0 | Unknown |
| 1 | Regular file |
| 2 | Directory |
| 3 | Character device |
| 4 | Block device |
| 5 | FIFO |
| 6 | Socket |
| 7 | Symbolic link |

### Deletion Behavior

When a file is deleted, its directory entry is not zeroed. The `rec_len` of the preceding active entry is expanded to bridge over the deleted entry, consuming its space. This is why recovery tools can sometimes find deleted files.

### Hash Tree (HTree) Indexing

When a directory outgrows a single block, it transitions to an indexed HTree if `EXT4_INDEX_FL` (`0x1000`) is set on the directory's inode. The HTree is a constant-depth B-tree keyed by 32-bit hashes of filenames.

**Root node structure (`dx_root`)** -- located in the first logical data block:

| Offset | Size | Description |
|--------|------|-------------|
| `0x00` | 4 | Fake inode (0 -- backward compat with ext2) |
| `0x04` | 2 | Fake rec_len (12 -- hides HTree from legacy) |
| `0x06` | 1 | Fake name_len (0) |
| `0x07` | 1 | Fake file_type (0) |
| `0x08` | 4 | Dot entry (. → self inode) |
| `0x0C` | 4 | Dot-dot entry (.. → parent inode) |
| `0x14` | 4 | Reserved |
| `0x18` | 1 | Hash version (0=legacy, 1=half_md4, 2=tea, 3=half_md4_unsigned, 4=tea_unsigned) |
| `0x19` | 1 | Length of each dx_entry (8) |
| `0x1A` | 1 | Tree depth (indirect levels) |
| `0x1B` | 1 | Unused flags |
| `0x1C` | 2 | Limit (max entries) |
| `0x1E` | 2 | Count (active entries) |
| `0x20` | 4 | First block pointer (block 0, always) |
| `0x24` | 8× | Array of `dx_entry` (hash, block) pairs |

**Entry structure (`dx_entry`):**

| Offset | Size | Description |
|--------|------|-------------|
| `0x00` | 4 | Hash value |
| `0x04` | 4 | Block number containing matching entries |

### Filename Lookup Algorithm

1. Hash the target filename using the algorithm from `dx_root` header (typically Half MD4)
2. Binary search the `dx_entry` array for the bounding hash range
3. Read the referenced leaf block
4. Perform linear scan of `ext4_dir_entry_2` entries within that block
5. Compare filenames character-by-character

---

## 9. The JBD2 Journaling Subsystem and Crash Resilience

The Journaling Block Device 2 (JBD2) operates as a distinctly independent subsystem that guarantees atomic metadata updates. JBD2 writes transactions sequentially to an isolated journal log before checkpointing them to their final physical locations.

### Journal Modes

| Mode | Description | Performance | Safety |
|------|-------------|-------------|--------|
| `data=ordered` (default) | Only metadata journaled. Data blocks flushed before metadata commit. | High | High |
| `data=journal` | Both metadata and data journaled (written twice). | Low | Maximum |
| `data=writeback` | Only metadata journaled. No data ordering guarantee. | Maximum | Low |

### Journal Sizing

| Filesystem Size | Journal Size |
|----------------|-------------|
| < 8 MiB | No journal |
| 8 MiB – 128 MiB | 4 MiB |
| 128 MiB – 2 GiB | Scales proportionally |
| > 128 GiB | 1 GiB (maximum) |

### Journal Header Structure

Every JBD2 block begins with a 12-byte header:

```c
struct journal_header_t {
    uint32_t h_magic;      /* 0xC03B3998 (big-endian!) */
    uint32_t h_blocktype;  /* Block type (see table below) */
    uint32_t h_sequence;   /* Transaction sequence ID */
};
```

> [!WARNING]
> All JBD2 structures are in **big-endian** byte order, unlike the rest of ext4 which is little-endian.

### Journal Block Types

| Type | Value | Description |
|------|-------|-------------|
| Descriptor | 1 | Contains array of `journal_block_tag_t` -- maps journal blocks to destination blocks |
| Commit | 2 | Marks atomic completion of a transaction (32 bytes with timestamp + checksum) |
| Superblock v1 | 3 | Journal superblock (legacy) |
| Superblock v2 | 4 | Journal superblock (modern, with feature flags) |
| Revocation | 5 | Array of block numbers whose prior journal entries should be ignored during replay |

### Journal Superblock

Located at block 0 of the journal file. Key fields:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 12 | header | Standard header (magic, type=3/4, sequence) |
| `0x0C` | 4 | `s_blocksize` | Journal block size (matches filesystem block size) |
| `0x10` | 4 | `s_maxlen` | Total blocks in journal |
| `0x14` | 4 | `s_first` | First usable block in journal |
| `0x18` | 4 | `s_sequence` | Sequence number of first expected transaction |
| `0x1C` | 4 | `s_start` | Block number of first transaction's log start |
| `0x20` | 4 | `s_errno` | Error number, as set by `jbd2_journal_abort()` |
| `0x24` | 4 | `s_feature_compat` | Compatible features |
| `0x28` | 4 | `s_feature_incompat` | Incompatible features (notably `JBD2_FEATURE_INCOMPAT_64BIT`) |
| `0x2C` | 4 | `s_feature_ro_compat` | Read-only features |
| `0xFC` | 4 | `s_checksum` | Journal superblock checksum |

### Descriptor Block Tags (`journal_block_tag_t`)

Each tag identifies the final destination of a subsequent journal data block:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 4 | `t_blocknr` | Destination block number (lower 32 bits) |
| `0x04` | 2 | `t_checksum` | CRC32C of the UUID + sequence + data block (lower 16 bits) |
| `0x06` | 2 | `t_flags` | Flags (see below) |
| `0x08` | 4 | `t_blocknr_high` | Destination block (upper 32 bits, if 64-bit journal) |

**Tag flags:**

| Flag | Value | Description |
|------|-------|-------------|
| `JBD2_FLAG_ESCAPE` | `0x01` | Data block's magic was escaped (replaced with zeros to prevent confusion) |
| `JBD2_FLAG_SAME_UUID` | `0x02` | This tag shares UUID with previous (UUID not repeated) |
| `JBD2_FLAG_DELETED` | `0x04` | This block has been revoked |
| `JBD2_FLAG_LAST_TAG` | `0x08` | Last tag in this descriptor block |

---

## 10. Fast Commits and the JBD2 Recovery State Machine

### Fast Commits (`JBD2_FEATURE_INCOMPAT_FAST_COMMIT`)

Instead of logging entire 4 KiB metadata blocks, Fast Commits log precise deltas using Tag Length Value (TLV) encodings:

| Tag | Operation |
|-----|-----------|
| `EXT4_FC_TAG_ADD_RANGE` | Append an extent |
| `EXT4_FC_TAG_DEL_RANGE` | Remove an extent |
| `EXT4_FC_TAG_CREAT` | Create a new directory entry |
| `EXT4_FC_TAG_LINK` | Create a hard link |
| `EXT4_FC_TAG_UNLINK` | Remove a directory entry |
| `EXT4_FC_TAG_INODE` | Update inode metadata |
| `EXT4_FC_TAG_PAD` | Padding (no-op) |
| `EXT4_FC_TAG_TAIL` | End of fast commit block (with CRC32C) |

Fast commits are **idempotent** -- they can be safely replayed multiple times. A traditional full commit is forced when the fast commit area fills, a timer expires, or an operation is too complex for TLV representation.

### Recovery Algorithm

When an unclean filesystem is mounted, the driver must invoke recovery:

**Pass 1 -- Scan Phase:**
The kernel iterates from the journal's `j_tail` block. It verifies checksums of all Descriptor and Commit blocks to identify the highest valid transaction ID, mapping the exact boundaries of the recoverable log.

**Pass 2 -- Revoke Phase:**
The log is scanned again to parse all Revocation Blocks. The driver builds an in-memory hash table tracking block numbers that have been revoked in subsequent transactions.

**Pass 3 -- Replay Phase:**
The log is traversed a final time. Any data block mapped by a valid Descriptor tag is checkpointed to its filesystem destination, *unless* its target block number exists in the Revocation hash table. Once replay hits the final valid Commit Block, the filesystem is restored to a consistent state.

---

## 11. Cryptographic Metadata Checksumming

Ext4 integrates comprehensive metadata checksumming (`metadata_csum`, `RO_COMPAT` flag `0x0400`) using **CRC32C** to detect silent bit rot and corruption.

### Checksum Seed

The checksum algorithm is seeded with the filesystem UUID. This prevents metadata copied from one volume from appearing valid on another. When `INCOMPAT_CSUM_SEED` is set, the seed is stored directly in `s_checksum_seed`, allowing UUID changes without recalculating all checksums.

```c
uint32_t seed = s_checksum_seed;  /* or CRC32C(~0, s_uuid, 16) if CSUM_SEED not set */
```

### Checksummed Structures

| Structure | Checksum Location | Fields Covered |
|-----------|------------------|----------------|
| Superblock | `s_checksum` at offset `0x3FC` | Entire Superblock (field zeroed during calculation) |
| Group Descriptor | `bg_checksum` | UUID + group number + descriptor data |
| Inode | `i_checksum_lo` + `i_checksum_hi` | UUID + inode number + generation + inode data |
| Directory block | `dx_tail` (last 8 bytes) | All directory entries in the block |
| Extent block | `ext4_extent_tail` (last 4 bytes) | Extent header + all entries |
| Bitmap | In Group Descriptor | Block/inode bitmap data |

> [!CAUTION]
> If a checksum verification fails during any read operation, the driver must immediately halt processing of that structure, return an I/O error, and flag the filesystem as corrupted to prevent propagation of compromised data.

---

## 12. Legacy Backward Compatibility: ext2 and ext3 Specifications

A custom ext4 driver is fully capable of mounting legacy ext2 and ext3 filesystems natively.

### Legacy Block Addressing: Indirect Block Map

Before extent trees, ext2/ext3 used an indirect block addressing scheme. The 60-byte `i_block` array contains 15 × 4-byte block pointers:

| Index | Type | Description |
|-------|------|-------------|
| 0–11 | Direct | Point directly to the first 12 data blocks |
| 12 | Single Indirect | Points to a block containing an array of direct pointers |
| 13 | Double Indirect | Points to a block of single-indirect pointers |
| 14 | Triple Indirect | Points to a block of double-indirect pointers |

**Maximum file size** with 4 KiB blocks and indirect addressing:

```
Direct:           12 × 4 KiB                           =    48 KiB
Single Indirect:  1024 × 4 KiB                         =     4 MiB
Double Indirect:  1024 × 1024 × 4 KiB                  =     4 GiB
Triple Indirect:  1024 × 1024 × 1024 × 4 KiB           =     4 TiB
Total:                                                  ≈   4.004 TiB
```

### Key Differences Across ext2/ext3/ext4

| Feature | ext2 | ext3 | ext4 |
|---------|------|------|------|
| Journaling | ❌ None | ✅ JBD | ✅ JBD2 (64-bit) |
| Block addressing | Indirect (32-bit) | Indirect (32-bit) | Extent tree (48-bit) |
| Inode size | 128 bytes (fixed) | 128 bytes (fixed) | 256 bytes (default) |
| Max volume size | 16 TiB | 16 TiB | 1 EiB |
| Max file size | 2 TiB | 2 TiB | 16 TiB (4K blocks) |
| Directory indexing | Linear only | HTree optional | HTree default |
| Timestamps | 32-bit (seconds) | 32-bit (seconds) | 64-bit (nanoseconds) |
| Metadata checksums | ❌ | ❌ | ✅ CRC32C |
| Delayed allocation | ❌ | ❌ | ✅ |
| Multiblock allocation | ❌ | ❌ | ✅ mballoc |
| Inline data | ❌ | ❌ | ✅ |
| Flex block groups | ❌ | ❌ | ✅ |
| Fast commits | ❌ | ❌ | ✅ |

---

## 13. Strategic Implementation Roadmap for Custom Operating Systems

### Phase 1: Minimal Read-Only Architecture

1. **Mount Verification:** Read block 0 (or 1), verify `0xEF53` magic at offset `0x38`. Read `s_feature_incompat` -- reject mount if unsupported flags are present.

2. **Geometry Calculation:** Compute block size via `1024 << s_log_block_size`. Parse Superblock for GDT location and `s_desc_size`.

3. **Root Inode Fetch:** Locate the Inode Table for Block Group 0 via the GDT. Fetch **Inode 2** (root directory).

4. **Path Resolution:** Read the `i_block` extent tree for Inode 2. Cache leaf extents in memory. Read directory data blocks. Linear scan `ext4_dir_entry_2` entries to match target filename.

5. **Payload Extraction:** Once the target file's inode is identified, parse its extent tree into a sorted memory cache. Execute physical disk reads using the mapped logical-to-physical offsets.

### Phase 2: Metadata Manipulation and Journaling Readiness

Before implementing write support, the driver must possess the capability to:

- Safely manipulate the Extent Tree (splitting leaf nodes, updating parent indices when files are appended)
- Balance the HTree (handling hash collisions, provisioning new leaf blocks)
- Implement the full JBD2 transactional state machine

> [!CAUTION]
> Write operations must **never** be enabled without a completely functional JBD2 layer. Writing directly to the disk without journaling invites instantaneous, irrecoverable corruption upon a kernel panic or unexpected power failure.

Implementing JBD2 requires:
- Building the transaction state machine
- Constructing descriptor tag arrays
- Implementing the multi-pass recovery algorithm (Scan → Revoke → Replay)
- Testing recovery to ensure the filesystem can heal itself upon reboot
