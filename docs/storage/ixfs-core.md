<!-- docs: covers=todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md sources=include/kernel/fs/ixfs.h,src/kernel/fs/ixfs/ixfs_internal.h,src/kernel/fs/ixfs/ixfs_core.c,src/kernel/fs/ixfs/ixfs_inode.c,src/kernel/fs/ixfs/ixfs_format.c,src/kernel/fs/ixfs/ixfs_ops.c,src/kernel/fs/partition.c,tools/mkfs-ixfs.c,src/kernel/test/test_ixfs.c reviewed=2026-09-28 order=6 -->
# IXFS Core

## What is it?

IXFS is Impossible OS's own filesystem and the one `C:` runs on. It uses 4 KiB blocks, extents, a write-ahead journal, CRC32C checksums on data blocks and copy-on-write snapshots. This roadmap first verifies each of those subsystems, then grows the inode from 128 to 256 bytes (format version 3) so IXFS can carry what Windows programs expect from NTFS: alternate data streams, security descriptors, hard links, symbolic links, extended attributes and object IDs. None of its eleven sections is complete; the shipped format is version 2.

## How does it work?

**On-disk layout.** [`ixfs.h`](../../include/kernel/fs/ixfs.h) defines it. Block 0 holds a 512-byte superblock (magic `0x49584653`, "IXFS") whose first 112 bytes are covered by a CRC32C. After it come the block bitmap, the checksum table, the inode table, the 16-block journal, the reference-count table and the snapshot table, and then the data blocks; each region's start is recorded in the superblock (`s_bitmap_start`, `s_checksum_start`, `s_inode_start`, `s_journal_start`, `s_refcount_start`, `s_snapshot_start`, `s_data_start`). An inode is 128 bytes, 32 to a block: size, times in seconds, four inline extents and a pointer to an overflow extent block. A directory entry is 256 bytes (a 4-byte inode number and a 252-byte name), so names are up to 251 characters; a longer name is cut short rather than refused. A volume formats with 256 inodes by default (`IXFS_DEFAULT_INODES` in [`ixfs_internal.h`](../../src/kernel/fs/ixfs/ixfs_internal.h)).

**Mounting `C:`.** At boot, `try_mount_ixfs_as_c()` in [`partition.c`](../../src/kernel/fs/partition.c) calls `ixfs_init()` in [`ixfs_format.c`](../../src/kernel/fs/ixfs/ixfs_format.c) on the active A/B slot. `ixfs_init()` checks the magic and version, mounts a version 1 volume read-only and refuses any other version, loads the bitmap and block groups, replays the journal and loads the snapshot and checksum tables. A superblock checksum mismatch is only a warning. Up to four IXFS volumes can mount at once, each with a 64-block cache.

**Names.** `ixfs_strcmp()` in [`ixfs_core.c`](../../src/kernel/fs/ixfs/ixfs_core.c) compares names case-insensitively (ASCII only) and returns 1 when they are equal, and the directory hash in [`ixfs_inode.c`](../../src/kernel/fs/ixfs/ixfs_inode.c) folds to lower case, so `C:\Readme.txt` and `C:\README.TXT` are the same file. There is no per-volume case-sensitive switch.

**What version 2 cannot hold.** The inode has no room for Windows attributes, a creation time separate from change time, streams, a security descriptor ID or an object ID. Every file has a link count of 1 and there is no hard-link call. `rename` with replace-existing is refused (`rename: REPLACE_EXISTING not supported (refused)` in [`ixfs_ops.c`](../../src/kernel/fs/ixfs/ixfs_ops.c)).

**Building images.** The host tool [`mkfs-ixfs.c`](../../tools/mkfs-ixfs.c) writes version 2 images for the boot disk; the kernel's `ixfs_format()` writes the same layout.

```mermaid
flowchart LR
    SB[superblock + CRC32C] --> BM[block bitmap]
    BM --> CK[checksum table]
    CK --> IT[inode table, 128 B inodes]
    IT --> J[journal]
    J --> RS[refcount + snapshot tables]
    RS --> D[data blocks via extents]
```

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `ixfs_init()`, `ixfs_get_driver()`, `ixfs_get_root()` | Mount and VFS registration ([`ixfs_format.c`](../../src/kernel/fs/ixfs/ixfs_format.c)) |
| `ixfs_format()` | Create a version 2 volume |
| `struct ixfs_superblock`, `struct ixfs_inode`, `struct ixfs_dir_entry` | On-disk structures, size-checked by `_Static_assert` ([`ixfs.h`](../../include/kernel/fs/ixfs.h)) |
| `mkfs-ixfs -o <img> -s <size> [-l <label>] [--populate <dir>]` | Host image builder ([`mkfs-ixfs.c`](../../tools/mkfs-ixfs.c)) |

## How do I use it?

Every boot mounts `C:` from IXFS. The serial log shows `A/B: mounted slot <n> as C:`, and at debug level the volume summary `IXFS: "<label>" v2, <n> MiB, <free>/<total> blocks free, <k> inodes`. `make test-fs` runs the IXFS layout suites in [`test_ixfs.c`](../../src/kernel/test/test_ixfs.c) (superblock size and offsets, inode size) and the repair-pass suites described in [IXFS Advanced Storage](ixfs-advanced.md).

## What is not implemented yet?

- **A verification pass** over the eight subsystems, including two stale comments in `ixfs.h` (it still describes indirect pointers and a 64-byte directory entry) ([Subsystem Verification](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#1-subsystem-verification-sonnet)).
- **The 256-byte version 3 inode** ([Extended Inode v3](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#2-extended-inode-v3-opus)) and its **format tool** ([Format Tool v3](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#10-format-tool-v3-opus)).
- **A named case-folding compare with a per-mount override** ([Case-Insensitive Paths](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#3-case-insensitive-paths-sonnet)).
- **Windows file features**: [alternate data streams](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#4-alternate-data-streams-ads-opus), [security descriptors](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#5-security-descriptors-opus), [hard links](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#6-hard-links-sonnet), [symbolic links and reparse points](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#7-symbolic-links--reparse-points-opus), [extended attributes](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#8-extended-attributes-sonnet) and [object IDs](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#9-object-ids-sonnet).
- **A parity gate** that checks all of it against NTFS behaviour ([Parity Gate](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md#11-parity-gate-sonnet)).

## How does it compare with Windows 11 and Linux?

NTFS on Windows 11 has streams, `$Secure` descriptors, hard links, reparse points, extended attributes and object IDs, but no per-block checksums. Btrfs on Linux checksums every block and scrubs, and ext4 offers optional per-directory case folding. IXFS already checksums data blocks and folds case, and plans to carry the NTFS feature set in one 256-byte inode.

## See also

- [IXFS core roadmap](../../todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md)
- [IXFS Advanced Storage](ixfs-advanced.md)
- [Volume Management and Auto-mount](volume-management.md)
- [A/B Boot and Rollback](../boot/ab-boot-rollback.md)
- [Storage](index.md)
