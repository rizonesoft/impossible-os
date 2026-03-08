# IXFS (Impossible X FileSystem) — On-Disk Format Specification

> Version 1 — Impossible OS native filesystem for the system partition (C:\)

## Overview

IXFS is a custom Unix-inspired filesystem designed for Impossible OS. It uses
4 KiB blocks (matching x86-64 page size), a fixed inode table, bitmap-based
allocation, and supports files up to ~4 GiB via direct + single-indirect +
double-indirect block pointers.

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
| 16     | 4    | `i_ctime`     | Creation timestamp (seconds since epoch) |
| 20     | 4    | `i_mtime`     | Modification timestamp |
| 24     | 4    | `i_atime`     | Access timestamp |
| 28     | 48   | `i_direct[12]`| 12 direct block pointers |
| 76     | 4    | `i_indirect`  | Single-indirect block pointer |
| 80     | 4    | `i_dindirect` | Double-indirect block pointer |
| 84     | 4    | `i_reserved`  | Zeroed, pads to 128 bytes |

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
**64-byte** directory entries:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0      | 4    | `d_inode` | Inode number (`0` = free/deleted entry) |
| 4      | 252  | `d_name`  | Filename (null-terminated, max 251 chars) |

- 64 entries per 4 KiB block
- Deleted entries have `d_inode = 0`
- No `.` or `..` entries stored (they are synthesized by the driver)

---

## Formatting

`ixfs_format()` creates a fresh filesystem:

1. Zero all blocks
2. Write superblock at block 0
3. Initialize block bitmap (mark metadata blocks as used)
4. Allocate inode 1 as root directory (`IXFS_S_DIR | IXFS_PERM_DIR`)
5. Allocate one data block for root directory (empty)

---

## Implementation Files

| File | Purpose |
|------|---------|
| `include/kernel/fs/ixfs.h` | On-disk structures and constants |
| `src/kernel/fs/ixfs.c` | Driver: format, mount, VFS operations |
