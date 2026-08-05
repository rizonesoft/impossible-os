---
schema_version: 1
id: ixfs-mount
domain: 14-host-tools
status: active
title: "TODO-02 -- IXFS Mount (Linux)"
---

# TODO-02 -- IXFS Mount (Linux)

> **Goal:** Mount IXFS partitions from Linux so developers can browse Impossible OS filesystems from the host -- read logs, inspect files, copy assets, without booting the OS. Also provides a mount script for USB boot drives.

> [!IMPORTANT]
> This is an **SDK tool**, not part of the Impossible OS kernel. Uses libfuse3 on Linux to mount IXFS partitions as regular directories. Shared IXFS parsing code (`ixfs-core.c`) with platform-independent disk I/O.

## Inputs

- [`src/kernel/fs/ixfs/ixfs_core.c`](../../src/kernel/fs/ixfs/ixfs_core.c) -- IXFS superblock, mount, metadata
- [`src/kernel/fs/ixfs/ixfs_inode.c`](../../src/kernel/fs/ixfs/ixfs_inode.c) -- inode read/write
- [`src/kernel/fs/ixfs/ixfs_extent.c`](../../src/kernel/fs/ixfs/ixfs_extent.c) -- extent-based block mapping
- [`src/kernel/fs/ixfs/ixfs_ops.c`](../../src/kernel/fs/ixfs/ixfs_ops.c) -- file operations (read, write, readdir)
- [`src/kernel/fs/ixfs/ixfs_alloc.c`](../../src/kernel/fs/ixfs/ixfs_alloc.c) -- block allocator
- [`include/kernel/fs/ixfs.h`](../../include/kernel/fs/ixfs.h) -- on-disk structures
- [`tools/mkfs-ixfs.c`](../../tools/mkfs-ixfs.c) -- IXFS formatter (reference for on-disk layout)
- → XREF: `14-host-tools/TODO-01-sdk-build-system.md` -- SDK build system discovers and builds this tool
- → XREF: `14-host-tools/TODO-06-disk-inspect.md` -- shares IXFS core parser (`ixfs-core.c`); coordinate struct changes
- → XREF: `14-host-tools/TODO-07-ixfs-fsck.md` -- shares IXFS core parser; coordinate struct changes

## Outcome

- `ixfs-mount build/system-disk.img:2 /mnt/ixfs` -- mount from disk image
- `ixfs-mount /dev/sdb:2 /mnt/ixfs` -- mount from device
- `./mount-ixfs-usb.sh` -- auto-detect USB drive, mount IXFS partition
- `fusermount -u /mnt/ixfs` -- unmount
- Read-write support: create, write, delete, rename files from host

## Source Layout

```
sdk/src/ixfs-mount/
  ├── ixfs-core.c           # Shared: superblock, inode, extent, readdir, write ops
  ├── ixfs-core.h           # Shared: IXFS API
  ├── ixfs-structs.h        # On-disk structures (from include/kernel/fs/ixfs.h, host types)
  ├── ixfs-disk.c           # Raw disk/image I/O with GPT parsing
  ├── ixfs-disk.h           # Disk I/O API
  ├── ixfs-fuse-linux.c     # Linux: libfuse3 callbacks + main
  ├── test_ixfs_core.c      # Dev test (not shipped)
  └── Makefile              # Linux build (gcc + libfuse3)

sdk/scripts/
  └── mount-ixfs-usb.sh     # Auto-detect USB, mount IXFS partition

sdk/tools/                  # Compiled output (gitignored)
  └── ixfs-mount            # Linux binary
```

> [!NOTE]
> `ixfs-structs.h` is a COPY of on-disk structures from kernel headers, using `<stdint.h>` instead of kernel types. Keep in sync manually -- any IXFS format change must be reflected here.

## Implementation Order

| ⭐  | Order | Deliverable                     | Depends On | Status |
| --- | :---: | ------------------------------- | ---------- | :----: |
| 💎  |   1   | Shared IXFS core parser         | --         |  [x]   |
| 💎  |   2   | Disk I/O layer with GPT         | §1         |  [x]   |
| 💎  |   3   | 🐧 Linux FUSE mount (read-only) | §2         |  [x]   |
| 💎  |   4   | Write support                   | §3         |  [x]   |
| 💎  |   5   | USB auto-mount script           | §3         |  [x]   |

---

## 1. Shared IXFS Core Parser
Port the IXFS on-disk structure parsing from kernel code to a standalone library. No kernel dependencies -- uses `<stdint.h>`, `<string.h>`, `<stdio.h>`.

**Files:** `sdk/src/ixfs-mount/ixfs-core.c`, `ixfs-core.h`, `ixfs-structs.h`

- [x] Copy on-disk structures from `include/kernel/fs/ixfs.h` to `ixfs-structs.h`
- [x] `ixfs_open(disk_ctx)` -- read and validate superblock
- [x] `ixfs_read_inode(ino)` -- read inode from inode table
- [x] `ixfs_extent_lookup(inode, file_block)` -- resolve file block to disk block
- [x] `ixfs_readdir(inode, callback)` -- enumerate directory entries
- [x] `ixfs_lookup(parent_ino, name)` -- find inode by name in directory
- [x] `ixfs_read_data(inode, offset, buf, len)` -- read file data
- [x] Commit: `"sdk: ixfs-mount shared IXFS parser -- no kernel dependencies"`

**Test checkpoint (Linux):**
- `test_ixfs_core build/system-disk.img 2` parses superblock, lists root directory

## 2. Disk I/O Layer with GPT
Abstracted raw sector read/write with GPT partition parsing.

**Files:** `sdk/src/ixfs-mount/ixfs-disk.c`, `ixfs-disk.h`

- [x] `disk_open(path, partition_index)` -- open image, locate partition via GPT
- [x] `disk_read_sectors(lba, count, buf)` -- read sectors within partition
- [x] `disk_write_sectors(lba, count, buf)` -- write sectors
- [x] `disk_close()` -- flush and close
- [x] GPT parser: find partition by index, get start LBA and size
- [x] Partition index 0: treat entire file as raw IXFS volume (no GPT)
- [x] Commit: `"sdk: ixfs-mount platform disk I/O with GPT parser"`

**Test checkpoint (Linux):**
- `disk_open("build/system-disk.img", 2)` succeeds -- GPT parsed, partition 2 found

## 3. Linux FUSE Mount (Read-Only)
Implement libfuse3 callbacks for read-only mounting.

**Files:** `sdk/src/ixfs-mount/ixfs-fuse-linux.c`, `Makefile`

- [x] `fuse_getattr` -- stat from inode
- [x] `fuse_readdir` -- enumerate directory
- [x] `fuse_open` -- validate file access
- [x] `fuse_read` -- read file data via extent lookup
- [x] Command: `ixfs-mount build/system-disk.img:2 /mnt/ixfs`
- [x] `fusermount -u /mnt/ixfs` for unmount
- [x] Makefile: conditional build -- skips ixfs-mount if libfuse3-dev missing
- [x] Commit: `"sdk: ixfs-mount Linux FUSE read-only mount"`

**Test checkpoint (Linux):**
- `./ixfs-mount build/system-disk.img:2 /mnt/ixfs` -- mounts without error
- `ls /mnt/ixfs/` lists root directory (Impossible/, Users/, etc.)
- `fusermount -u /mnt/ixfs` -- clean unmount

## 4. Write Support
Add create, write, delete, rename operations.

**Files:** `sdk/src/ixfs-mount/ixfs-core.c`, `ixfs-fuse-linux.c`

- [x] `ixfs_write_data(inode, offset, buf, len)` -- write file data, allocate blocks
- [x] `ixfs_create(parent_ino, name, type)` -- create file or directory
- [x] `ixfs_delete(parent_ino, name)` -- remove entry, free blocks
- [x] `ixfs_rename(old_parent, old_name, new_parent, new_name)`
- [x] Block allocator: port from `ixfs_alloc.c`
- [x] Flush dirty inodes + superblock on unmount
- [x] Wire into Linux FUSE backend (write, create, mkdir, unlink, rmdir, rename, truncate)
- [x] Commit: `"sdk: ixfs-mount read-write support"`

**Test checkpoint (Linux):**
- Mount R/W, create `/mnt/ixfs/test-write.txt` with known content
- Unmount, re-mount, verify file exists and content matches
- Delete file from host, unmount, re-mount, verify file gone

## 5. USB Auto-Mount Script
Script to detect USB drives with IXFS partitions and mount them automatically.

**Files:** `sdk/scripts/mount-ixfs-usb.sh`

- [x] Scan `/sys/block/sd*` and `/sys/block/nvme*` for USB/removable drives
- [x] For each partition: read IXFS magic (0x49584653) at offset 0
- [x] Mount at `/mnt/ixfs` via ixfs-mount
- [x] Log: `Mounted IXFS from /dev/sdb2 at /mnt/ixfs`
- [x] Unmount: `fusermount -u /mnt/ixfs`
- [x] Commit: `"sdk: USB auto-mount script for IXFS"`

**Test checkpoint (Linux):**
- Plug USB boot drive, run `./mount-ixfs-usb.sh` -- output: `Mounted IXFS from /dev/sdb2 at /mnt/ixfs`
- `ls /mnt/ixfs/` shows IXFS root directory
- `fusermount -u /mnt/ixfs` -- clean unmount

---

## OS Comparison

| ⭐  | Feature               | 🪟 Win11           | 🐧 Linux          | 🚀 Impossible OS         |
| --- | --------------------- | ------------------ | ----------------- | ------------------------ |
| 💎  | Cross-OS FS mount     | ✅ ext2fsd         | ✅ ntfs-3g        | ✅ §3 IXFS FUSE mount    |
| ⭐  | USB auto-mount        | ❌ No custom FS    | ❌ No custom FS   | ✅ §5 detect + mount USB |
| ⭐  | Image:partition mount | ❌ Manual losetup  | ❌ Manual losetup | ✅ §2 `img:N` one-cmd    |
| ⭐  | R/W from day one      | ⚠️ ext2fsd corrupts | ⚠️ ntfs-3g slow    | ✅ §4 write support      |

## Verification

- [x] Linux: `ixfs-mount build/system-disk.img:2 /mnt/ixfs && ls /mnt/ixfs/`
- [x] Write: create file from host, unmount, re-mount, file persists
- [x] USB: plug USB drive, run mount script, IXFS mounted and browsable
