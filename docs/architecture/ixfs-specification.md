# IXFS (Impossible X FileSystem) — On-Disk Format Specification

> Version 1 — Impossible OS native filesystem for the system partition (C:\)

## Overview

IXFS is a custom Unix-inspired filesystem designed for Impossible OS. It uses
4 KiB blocks (matching x86-64 page size), a fixed inode table, bitmap-based
allocation, and supports files up to ~4 GiB via direct + single-indirect +
double-indirect block pointers.

Key design decisions:
- **Block device abstraction** — all I/O goes through the `blkdev` layer,
  not direct ATA calls. Works with any storage backend (ATA, AHCI, VirtIO).
- **Block group allocator** — in-memory optimization for locality-aware
  allocation with O(1) sequential writes.
- **No on-disk format dependency on features** — block groups, caching, and
  other optimizations are purely in-memory. The on-disk format is simple and stable.

## Disk Layout

```
Block 0             Superblock (512 useful bytes, padded to 4 KiB)
Block 1..N          Block bitmap (1 bit per block)
Block N+1..M        Inode table (32 inodes per block, 128 bytes each)
Block M+1..end      Data blocks (file and directory content)
```

All multi-byte fields are little-endian.

---

## Superblock (Block 0)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0      | 4    | `s_magic`        | `0x49584653` ("IXFS" in LE) |
| 4      | 4    | `s_version`      | Format version (currently 1) |
| 8      | 4    | `s_block_size`   | Block size in bytes (4096) |
| 12     | 4    | `s_total_blocks` | Total blocks on the volume |
| 16     | 4    | `s_free_blocks`  | Number of free blocks |
| 20     | 4    | `s_total_inodes` | Total inodes allocated |
| 24     | 4    | `s_free_inodes`  | Number of free inodes |
| 28     | 4    | `s_bitmap_start` | First block of block bitmap |
| 32     | 4    | `s_bitmap_blocks`| Number of blocks in bitmap |
| 36     | 4    | `s_inode_start`  | First block of inode table |
| 40     | 4    | `s_inode_blocks` | Number of blocks in inode table |
| 44     | 4    | `s_data_start`   | First data block |
| 48     | 4    | `s_root_inode`   | Inode number of root directory (always 1) |
| 52     | 32   | `s_volume_name`  | Volume label (null-terminated ASCII) |
| 84     | 428  | `s_reserved`     | Zeroed, pads to 512 bytes |

---

## Block Bitmap (Blocks 1..N)

- **1 bit per block**: `0` = free, `1` = allocated
- Bitmap size: `ceil(s_total_blocks / 8)` bytes, padded to whole blocks
- Number of bitmap blocks stored in `s_bitmap_blocks`
- The superblock, bitmap, and inode table blocks are always marked as allocated

---

## Inode Table (Blocks N+1..M)

Each inode is **128 bytes** (32 inodes per 4 KiB block).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0      | 2    | `i_mode`      | Type (upper 4 bits) + permissions (lower 9 bits) |
| 2      | 2    | `i_links`     | Hard link count |
| 4      | 2    | `i_uid`       | Owner user ID |
| 6      | 2    | `i_gid`       | Owner group ID |
| 8      | 4    | `i_size`      | File size in bytes |
| 12     | 4    | `i_blocks`    | Number of data blocks used |
| 16     | 4    | `i_ctime`     | Creation timestamp (seconds since boot) |
| 20     | 4    | `i_mtime`     | Modification timestamp |
| 24     | 4    | `i_atime`     | Access timestamp |
| 28     | 48   | `i_direct[12]`| 12 direct block pointers |
| 76     | 4    | `i_indirect`  | Single-indirect block pointer |
| 80     | 4    | `i_dindirect` | Double-indirect block pointer |
| 84     | 44   | `i_reserved`  | Zeroed, pads to 128 bytes |

### Type Flags (`i_mode` upper 4 bits)

| Value    | Type |
|----------|------|
| `0x8000` | Regular file |
| `0x4000` | Directory |

### Permission Bits (`i_mode` lower 9 bits)

Standard Unix-style rwx for user/group/other.

### Block Pointer Addressing

```
Logical block 0..11:    i_direct[n]              → data block
Logical block 12..1035: i_indirect → ptr[n-12]   → data block
Logical block 1036+:    i_dindirect → ptr1 → ptr2 → data block
```

| Level | Max blocks | Max file size |
|-------|-----------|---------------|
| Direct only (12) | 12 | 48 KiB |
| + Single indirect (+1024) | 1,036 | ~4 MiB |
| + Double indirect (+1,048,576) | 1,049,612 | ~4 GiB |

### Special Inodes

| Number | Purpose |
|--------|---------|
| 0      | Reserved (never used) |
| 1      | Root directory (`/` or `C:\`) |

---

## Free Inode Bitmap

Inode allocation uses the **same block bitmap mechanism** — inode 0 is
always reserved, inode 1 is the root directory. Free inodes are tracked
by checking `i_mode == 0` in the inode table. The superblock's
`s_free_inodes` counter is maintained for fast queries.

---

## Directory Entries

Directories are regular files whose data blocks contain an array of
**256-byte** directory entries:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0      | 4    | `d_inode` | Inode number (`0` = free/deleted entry) |
| 4      | 252  | `d_name`  | Filename (null-terminated, max 251 chars) |

- 16 entries per 4 KiB block
- Deleted entries have `d_inode = 0`
- `.` and `..` entries are stored in directory data blocks

---

## Block Group Allocator (In-Memory)

The block group allocator is a **purely in-memory** optimization — no
on-disk format changes. It divides the volume into logical groups for
locality-aware allocation.

### Group Parameters

| Constant | Value | Description |
|----------|-------|-------------|
| `IXFS_BLOCKS_PER_GROUP` | 32,768 | Blocks per group (128 MiB) |
| `IXFS_MAX_BLOCK_GROUPS` | 256 | Max groups (supports 32 GiB) |

### Group Descriptor (in-memory only, not stored on disk)

| Field | Size | Description |
|-------|------|-------------|
| `bg_start` | 4 bytes | First block number in this group |
| `bg_count` | 4 bytes | Total blocks in this group |
| `bg_free` | 4 bytes | Number of free blocks |
| `bg_next_free` | 4 bytes | Hint: next block to try allocating |

### Allocation Algorithm

1. Prefer the same group as the file's inode (locality)
2. Start scanning from `bg_next_free` hint
3. On allocation: advance hint to `block + 1` (O(1) sequential writes)
4. On free: regress hint if freed block is earlier
5. If group is full, try the next group (wraps around)

---

## Buffer Cache (In-Memory, Write-Back)

A 64-entry LRU cache sits between the filesystem code and the raw
`blkdev_read/write` calls. All block I/O goes through the cache.

### Cache Parameters

| Constant | Value | Description |
|----------|-------|-------------|
| `IXFS_CACHE_SIZE` | 64 | Number of cached 4 KiB blocks (256 KiB total) |

### Cache Entry

| Field | Description |
|-------|-------------|
| `block` | Block number (`0xFFFFFFFF` = unused slot) |
| `data[4096]` | Cached block data |
| `dirty` | `1` if modified but not yet written to disk |
| `lru_tick` | LRU counter (higher = more recently used) |

### Operation

- **Read hit**: Returns data from cache (zero disk I/O)
- **Read miss**: Reads from disk, inserts into cache (evicts LRU if full)
- **Write**: Updates cache entry, marks dirty (no disk I/O)
- **Eviction**: If evicted entry is dirty, flushes to disk first
- **Flush**: `ixfs_cache_flush()` writes all dirty entries to disk
- **Init**: `ixfs_cache_init()` called on format/mount, invalidates all entries

### Flush Points

Dirty cache entries are flushed to disk at these points:
- `ixfs_flush_bitmap()` — after writing bitmap blocks
- `ixfs_flush_superblock()` — after writing superblock
- On LRU eviction of a dirty entry

---

## Directory Hash Index (In-Memory)

For directories with more than 64 entries, IXFS builds an in-memory hash
index for O(1) average-case filename lookups.

### Parameters

| Constant | Value | Description |
|----------|-------|-------------|
| `IXFS_HASH_BUCKETS` | 128 | Number of hash buckets |
| `IXFS_HASH_THRESHOLD` | 64 | Entries before hash index activates |

### Hash Function

FNV-1a (Fowler–Noll–Vo) 32-bit hash:
- Offset basis: `0x811C9DC5`
- Prime: `0x01000193`
- Output modulo `IXFS_HASH_BUCKETS` gives the bucket index

### Operation

- **Small dirs** (≤ 64 entries): linear scan (no overhead)
- **Large dirs** (> 64 entries): hash index built lazily on first `finddir`
- **Lookup**: FNV-1a(name) → bucket → walk chain → verify name (handles collisions)
- **Invalidation**: hash index freed on `create` or `unlink` → rebuilt on next `finddir`

### Data Structures

```
ixfs_vnode.dir_hash → struct ixfs_dir_hash
    bucket[128]         → head index into nodes[]
    nodes[]             → (entry_index, next) chain
```


---

## Extent-Based Allocation

IXFS replaces legacy block pointers with extent-based allocation for
efficient contiguous block management.

### Extent Structure (12 bytes)

| Field | Type | Description |
|-------|------|-------------|
| `e_start` | `uint64_t` | First block number (64-bit) |
| `e_count` | `uint32_t` | Number of contiguous blocks |

### Inode Layout

- 4 inline extents per inode (48 bytes) — no extra I/O needed
- `i_extent_count` (uint8_t) — how many extents are active
- `i_extent_block` (uint64_t) — overflow tree block (0 = none)

### Operation

- **Sequential writes** merge into the last extent (O(1) — 1 extent for contiguous files)
- **Block lookup** walks inline extents to map file offset → disk block
- **Max capacity**: 64 TiB with 64-bit block numbers and 4K blocks
- **Overflow**: if >4 extents needed, an extent tree block is allocated (future)

---

## Write-Ahead Log (Journal)

IXFS provides crash safety through a circular write-ahead log that journals
metadata changes before applying them to their final disk locations.

### On-Disk Layout

| Area | Block(s) | Description |
|------|----------|-------------|
| Journal header | `s_journal_start` | Magic, head/tail pointers, sequence |
| Journal entries | `s_journal_start+1` ... `+15` | DATA and COMMIT records |

- Default: **16 blocks** (64 KiB), stored between inode table and data area
- Circular buffer with head/tail wrapped to journal bounds

### Journal Entry (4096 bytes)

| Field | Type | Description |
|-------|------|-------------|
| `je_txn_id` | `uint32_t` | Transaction ID |
| `je_type` | `uint32_t` | DATA (1) or COMMIT (2) |
| `je_target` | `uint32_t` | Target disk block |
| `je_checksum` | `uint32_t` | Additive checksum of data |
| `je_data` | `uint8_t[4080]` | First 4080 bytes of target block |

### Transaction API

- `ixfs_txn_begin(vol)` — allocate new transaction ID, mark active
- `ixfs_txn_write(vol, block, data)` — write DATA entry to journal
- `ixfs_txn_commit(vol)` — write COMMIT record, flush to final locations

### Metadata Journaling (Default)

Bitmap flushes and superblock writes are automatically journaled when a
transaction is active. This ensures inode/bitmap/superblock updates are
atomic across crashes.

### Recovery

On mount, `ixfs_journal_recover()` scans from tail to head:
- DATA entries with valid checksums → replayed to target blocks
- COMMIT records → confirm preceding DATA entries are valid
- Incomplete transactions (no COMMIT) → discarded

---

## Copy-on-Write + Snapshots

IXFS provides block-level Copy-on-Write (CoW) with refcounted blocks,
enabling zero-overhead point-in-time snapshots.

### Refcount Table

| Area | Block(s) | Description |
|------|----------|-------------|
| Refcount table | `s_refcount_start` .. `+1` | 1 byte per block (max 255 refs) |

- 2 blocks store uint8_t refcounts for all 8192 blocks
- Allocated blocks start at refcount 1; free blocks at 0
- When a snapshot is created, all data block refcounts are incremented

### CoW Mechanism

When writing to a block with `refcount > 1`:
1. Allocate a new block (refcount = 1)
2. Copy old data to new block
3. Write new data to new block
4. Update inode extent to point to new block
5. Decrement old block's refcount (free at 0)

### Snapshot Table

| Area | Block(s) | Description |
|------|----------|-------------|
| Snapshot table | `s_snapshot_start` | Up to 8 snapshot entries (64 bytes each) |

Each `ixfs_snapshot_entry` (64 bytes):
- `se_name[32]` — snapshot name
- `se_timestamp` — creation time
- `se_root_block` — block storing saved inode table copy
- `se_inode_blocks` — number of inode blocks saved
- `se_flags` — active/deleted

### Snapshot API

| Function | Description |
|----------|-------------|
| `ixfs_snapshot_create(name)` | Save inode table copy, increment all data block refcounts |
| `ixfs_snapshot_list()` | Enumerate active snapshots |
| `ixfs_snapshot_restore(name)` | Swap current inode table with saved copy |
| `ixfs_snapshot_delete(name)` | Decrement refcounts, free unreferenced blocks |

---

## Sparse File Support

IXFS supports sparse files natively through hole extents.

### Hole Extents

An extent with `e_start = 0` represents a **hole** — a region of the file
containing only zeroes, with no disk blocks allocated.

| Extent field | Value | Meaning |
|-------------|-------|---------|
| `e_start` | 0 | Hole (no disk allocation) |
| `e_count` | N | Number of file blocks in the hole |

- `ixfs_get_block()` returns 0 for blocks within hole extents
- `ixfs_file_read()` returns zeroes for holes (no disk I/O)
- `ixfs_file_write()` inserts a hole extent when writing past the end of mapped blocks
- `ixfs_stat()` reports both `logical_size` and `actual_blocks` (i_blocks)

Sparse files use significantly less space than their logical size:

```
logical_size = 8207 bytes (3 file blocks)
actual_blocks = 1 block   (only the written block)
```

---

## Inline Small Files

Files ≤ 48 bytes are stored directly in the inode's `i_extents[]` area, avoiding
block allocation entirely. This benefits small config files, symlinks, and metadata.

### Inline Storage Layout

When `IXFS_INLINE` (0x02) is set in `i_extent_flags`, the 48-byte `i_extents[4]`
array is reused as raw data storage:

| Inode field | Inline meaning |
|-------------|---------------|
| `i_extents[0..3]` | 48 bytes of file data |
| `i_extent_flags` | `IXFS_INLINE` (0x02) set |
| `i_extent_count` | unused (0) |
| `i_blocks` | 0 (no disk blocks) |
| `i_size` | actual data length (0–48) |

### Transparent Promotion

When a write causes `offset + size > 48`:

1. Save existing inline data (up to 48 bytes)
2. Clear `IXFS_INLINE` flag and zero the extent area
3. Allocate a disk block via `ixfs_add_block_to_extent()`
4. Copy saved data + new data into the block
5. Continue with normal extent-based write for remaining data

### Constants

```c
#define IXFS_INLINE      0x02   /* flag in i_extent_flags */
#define IXFS_INLINE_MAX  48     /* max inline bytes = sizeof(i_extents) */
```

---

## Per-Block Checksums

CRC32C (Castagnoli) checksums are computed on every data block write and verified
on every read from disk. This detects silent data corruption (bit rot).

### Checksum Table

A dedicated on-disk area stores one `uint32_t` CRC32C per block:

| Property | Value |
|----------|-------|
| Location | Between bitmap and inode table |
| Entry size | 4 bytes (uint32_t) |
| Entries per block | 1024 (4096 / 4) |
| For 8192-block volume | 8 blocks (32 KiB) |

### Superblock Self-Checksum

The `s_checksum` field stores CRC32C of superblock bytes 0–103. Recomputed on
every `ixfs_flush_superblock()` call, verified at mount time.

### ixfs_scrub()

Full-volume integrity scan: reads every allocated data block directly from disk,
computes CRC32C, compares against stored checksum. Returns the number of
corrupted blocks found (0 = clean).

### Constants

```c
vol->sb.s_checksum_start   /* first checksum table block */
vol->sb.s_checksum_blocks  /* number of checksum table blocks */
vol->sb.s_checksum         /* CRC32C of superblock bytes [0..103] */
```

---

## 64-Bit Block Addressing

IXFS v2 upgrades key fields to `uint64_t` for large volume support:

| Field | v1 | v2 | Max |
|-------|-----|-----|-----|
| `s_total_blocks` | uint32_t (16 TiB) | uint64_t (64+ TiB) | 2^64 blocks |
| `s_free_blocks` | uint32_t | uint64_t | — |
| `i_size` | uint32_t (4 GiB) | uint64_t (16 EiB) | — |
| `e_start` (extent) | uint64_t | uint64_t | unchanged |

### Backward Compatibility

- v1 volumes are detected by `s_version == 1` and mounted **read-only**
- All write operations check the `read_only` flag and return `-1`
- v2 format always uses `IXFS_VERSION = 2`

### Superblock CRC32C

The CRC32C range covers bytes 0–111 (was 0–103 in v1) to include the widened
`s_total_blocks` and `s_free_blocks` fields.

---

## Formatting

`ixfs_format(dev, label)` creates a fresh filesystem:

1. Zero all metadata blocks
2. Write superblock at block 0
3. Initialize block bitmap (mark metadata blocks as used)
4. Allocate inode 1 as root directory (`IXFS_S_DIR | IXFS_PERM_DIR`)
5. Allocate one data block for root directory (empty)
6. Initialize block group descriptors from bitmap

---

## Mounting

`ixfs_init(dev)` mounts an existing filesystem:

1. Read superblock from block 0, validate magic and version
2. Load block bitmap into memory (`kmalloc`)
3. Initialize block group descriptors from bitmap
4. Read root inode and create root VFS node

---

## VFS Integration

IXFS registers with the VFS as a filesystem driver:

| VFS Operation | IXFS Function | Description |
|---------------|---------------|-------------|
| open | `ixfs_file_open` | Open file/directory |
| close | `ixfs_file_close` | Close file/directory |
| read | `ixfs_file_read` | Read file data with block pointer traversal |
| write | `ixfs_file_write` | Write file data with block allocation |
| readdir | `ixfs_readdir` | List directory entries (skips deleted) |
| finddir | `ixfs_finddir` | Lookup file/directory by name |
| create | `ixfs_create` | Create file or directory |
| unlink | `ixfs_unlink` | Delete file or directory (empty check) |
| rename | `ixfs_rename` | Rename file or directory in-place |

### Auto-Mount

IXFS partitions are detected by `partition_mount_filesystems()` and
auto-mounted at `C:\` (system drive). This uses the `blkdev` layer —
partition sub-devices created during boot.

### First-Boot Setup

On first boot (empty C:\), `firstboot_setup()` in `main.c`:
1. Creates default directory hierarchy (`Impossible\`, `Users\`, `Programs\`)
2. Copies initrd files from `B:\` to `C:\Impossible\System\`

> This is temporary — to be replaced by an installer. Remove
> `firstboot.c/h` and the call in `main.c` once an installer exists.

---

## Multi-Volume Support

IXFS supports up to 4 concurrently mounted partitions (e.g., `C:\`, `E:\`).
All per-volume state is encapsulated in `struct ixfs_volume`.

### Volume Pool

| Constant | Value | Description |
|----------|-------|-------------|
| `IXFS_MAX_VOLUMES` | 4 | Max simultaneous IXFS mounts |

### Architecture

All internal functions receive `struct ixfs_volume *vol` as their first
parameter. VFS callbacks derive the volume pointer from the vnode:

```
node->fs_data → ixfs_vnode → vnode->vol → struct ixfs_volume
```

Each volume contains: superblock, block bitmap, block group descriptors,
buffer cache (64 entries), vnode pool (64 nodes), and a scratch buffer.

---

## Implementation Files

| File | Purpose |
|------|---------|
| `include/kernel/fs/ixfs.h` | On-disk structures, constants, block group struct |
| `src/kernel/fs/ixfs.c` | Driver: format, mount, block groups, VFS operations |
| `src/kernel/fs/firstboot.c` | First-boot hierarchy + initrd copy (temporary) |
| `include/kernel/fs/firstboot.h` | First-boot API |

---

## Feature Comparison

These features, taken together, make IXFS genuinely competitive:

| Feature            | FAT32 | exFAT  | ext2  | ext3  | ext4                | NTFS            | Btrfs     | **IXFS**                     |
|--------------------|-------|--------|-------|-------|---------------------|-----------------|-----------|------------------------------|
| Journaling         | [-]   | [-]    | [-]   | [x]   | [x]                 | [x]             | N/A (CoW) | [x] **implemented** (§5.7)   |
| Copy-on-Write      | [-]   | [-]    | [-]   | [-]   | [-]                 | [-]             | [x]       | [x] **implemented** (§5.8)   |
| Snapshots          | [-]   | [-]    | [-]   | [-]   | [-]                 | VSS (userspace) | [x]       | [x] **implemented** (§5.8)   |
| Inline small files | [-]   | [-]    | [-]   | [-]   | [-]                 | [x] (MFT)       | [x]       | [x] **implemented** (§5.9.2) |
| Extent-based       | [-]   | [-]    | [-]   | [-]   | [x]                 | [x]             | [x]       | [x] **implemented** (§5.6)   |
| Per-block checksum | [-]   | [-]    | [-]   | [-]   | [-] (metadata only) | [-]             | [x]       | [x] **implemented** (§5.9.3) |
| Sparse files       | [-]   | [-]    | [-]   | [-]   | [x]                 | [x]             | [x]       | [x] **implemented** (§5.9.1) |
| Block groups       | [-]   | [-]    | [x]   | [x]   | [x]                 | [-]             | [-]       | [x] **implemented**          |
| Compression        | [-]   | [-]    | [-]   | [-]   | [-]                 | [x]             | [x]       | [~] planned                  |
| Encryption         | [-]   | [-]    | [-]   | [-]   | [x] (fscrypt)       | [x] (EFS)       | [-]       | [~] planned                  |
| Hard links         | [-]   | [-]    | [x]   | [x]   | [x]                 | [x]             | [x]       | [~] planned                  |
| 64-bit addressing  | [-]   | [-]    | [-]   | [-]   | [x] (48-bit)        | [-]             | [x]       | [x] **implemented** (§5.9.4) |
| Symbolic links     | [-]   | [-]    | [x]   | [x]   | [x]                 | [x]             | [x]       | [~] planned                  |
| Dir hash index     | [-]   | [-]    | [-]   | [x]   | [x]                 | [x] (B+tree)    | [x]       | [x] **implemented** (§5.5.3) |
| Deduplication      | [-]   | [-]    | [-]   | [-]   | [-]                 | [-]             | [x]       | [-]                          |
| Online resize      | [-]   | [-]    | [-]   | [x]   | [x]                 | [x]             | [x]       | [-]                          |
| POSIX ACLs         | [-]   | [-]    | [x]   | [x]   | [x]                 | [x] (NTFS ACL)  | [x]       | [~] planned                  |
| Extended attrs     | [-]   | [-]    | [x]   | [x]   | [x]                 | [x] (streams)   | [x]       | [~] planned                  |
| Quotas             | [-]   | [-]    | [-]   | [-]   | [x]                 | [x]             | [x]       | [-]                          |
| Multi-device/RAID  | [-]   | [-]    | [-]   | [-]   | [-]                 | [-]             | [x]       | [-]                          |
| Defragmentation    | [-]   | [-]    | [-]   | [-]   | [x]                 | [x]             | [x]       | [-]                          |
| Unicode filenames  | [-]   | [x]    | [-]   | [-]   | [x] (UTF-8)         | [x] (UTF-16)    | [x]       | [~] planned                  |
| Nanosec timestamps | [-]   | [-]    | [-]   | [-]   | [x]                 | [x] (100ns)     | [x]       | [-] (seconds)                |
| Transactions       | [-]   | [-]    | [-]   | [-]   | [-]                 | [x] (TxF)       | [x] (CoW) | [x] **implemented** (§5.7)   |
| Max file/vol size  | 4 GiB | 128 PB | 2 TiB | 2 TiB | 16 TiB              | 16 TiB          | 16 EiB    | [x] **64 TiB** (§5.6)        |

**IXFS's unique identity**: A hybrid of ext4's block-group locality with
Btrfs-style CoW snapshots, plus mandatory per-block checksums. Designed
from scratch for Impossible OS with zero legacy baggage.
