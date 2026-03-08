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

## Implementation Files

| File | Purpose |
|------|---------|
| `include/kernel/fs/ixfs.h` | On-disk structures, constants, block group struct |
| `src/kernel/fs/ixfs.c` | Driver: format, mount, block groups, VFS operations |
| `src/kernel/fs/firstboot.c` | First-boot hierarchy + initrd copy (temporary) |
| `include/kernel/fs/firstboot.h` | First-boot API |

---

## Future Enhancements (Planned)

| Feature | Section | Status |
|---------|---------|--------|
| Extent-based allocation | §5.6 | 🔜 Planned |
| Journaling (WAL) | §5.7 | 🔜 Planned |
| Copy-on-Write + Snapshots | §5.8 | 🔜 Planned |
| Sparse files | §5.9.1 | 🔜 Planned |
| Inline small files | §5.9.2 | 🔜 Planned |
| Per-block checksums | §5.9.3 | 🔜 Planned |
| 64-bit block addressing | §5.9.4 | 🔜 Planned (target: 64 TiB max) |
| Host-side mkfs-ixfs tool | §5.11 | 🔜 Planned |
