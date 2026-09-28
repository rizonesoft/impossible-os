<!-- docs: covers=todo/05-storage-filesystems/TODO-09-ext4-readwrite.md sources=src/kernel/fs/partition.c,include/kernel/fs/partition.h,include/kernel/fs/vfs.h reviewed=2026-09-28 order=9 -->
# ext4

## What is it?

ext4 is the default filesystem of Ubuntu, Fedora, Debian and most other Linux distributions, so it is what a dual-boot machine's Linux partition and many external Linux disks use. Impossible OS recognises an ext2, ext3 or ext4 partition by its magic number but has no driver, so it never mounts one. This roadmap writes a read-write driver from scratch in twelve sections, none of them started.

## Why is it written from scratch?

Because the two existing implementations cannot be used. The Linux `fs/ext4` driver is GPL-2.0-only, and `lwext4`, the usual standalone choice, is GPL-2.0 (its licence file was checked on 2026-08-17). Neither is compatible with this project's GPL-3.0-only licence, so neither may be vendored, adapted or translated. [`CREDITS.md`](../../CREDITS.md) records this under "Sources that cannot be used". The on-disk format itself is documented and free to implement.

## How does it work?

**Today.** `probe_filesystem()` in [`partition.c`](../../src/kernel/fs/partition.c) reads sector 2 of a partition (byte 1024, where the superblock starts) and checks for the magic `0xEF53` at offset 56. A match is classed `PART_FS_EXT2` ([`partition.h`](../../include/kernel/fs/partition.h)) whatever the actual ext version, and the mount pass skips it: only IXFS, FAT32 and NTFS partitions get a drive letter.

**Planned design.** The roadmap reads before it writes, and replays the journal before it writes anything:

1. Superblock and block group descriptors, including the 64-bit layout.
2. The inode reader and the extent tree decoder, with the old indirect-block map kept as a fallback for ext3-era files.
3. Hashed (htree) directories and file reads, including small files stored inline in the inode.
4. JBD2 journal replay on mount. The roadmap makes this a hard gate: no write path is enabled until replay works, because writing to a dirty ext4 volume without replay corrupts it.
5. File writes inside JBD2 transactions, extent tree changes, directory writes, then create, delete and rename.
6. Extended attributes, VFS registration and a repair pass. A volume with an unknown incompatible feature flag is refused, because the driver cannot be sure it reads the layout correctly; an unknown read-only-compatible flag mounts the volume read-only.

```mermaid
flowchart LR
    SB[superblock + group descriptors] --> I[inode reader]
    I --> E[extent tree]
    E --> D[htree directories]
    D --> R[file read]
    R --> J[JBD2 replay]
    J --> W[journaled writes]
    W --> V[VFS registration + fsck]
```

## What are its interfaces?

None yet beyond the magic-number probe. The driver will register a `struct vfs_fs_driver` with `vfs_mount()` ([`vfs.h`](../../include/kernel/fs/vfs.h)) and replace the ext2 probe with an `ext4_probe()` in the planned probe chain ([Volume Management](volume-management.md)).

## How do I use it?

It cannot be used yet. A Linux partition on an attached disk is scanned and recognised, but gets no drive letter and cannot be read.

## What is not implemented yet?

Everything in the roadmap:

- **Reading**: [Superblock + Block Group Descriptors](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#1-superblock--block-group-descriptors-sonnet), [Inode Reader](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#2-inode-reader-sonnet), [Extent Tree Decoder](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#3-extent-tree-decoder-opus), [Directory htree + Readdir](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#4-directory-htree-b-tree--readdir-opus) and [File Read](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#5-file-read-sonnet).
- **The journal**: [JBD2 Journal Replay](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#6-jbd2-journal-replay-opus).
- **Writing**: [File Write](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#7-file-write-opus), [Extent Tree Write](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#8-extent-tree-write-opus), [Directory Write](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#9-directory-write-sonnet) and [Create / Delete / Rename](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#10-file-create--delete--rename-sonnet).
- **The rest**: [Extended Attributes](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#11-extended-attributes-sonnet) and [VFS Registration + Probe + fsck](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md#12-vfs-registration--probe--fsck-sonnet).

## How does it compare with Windows 11 and Linux?

Windows 11 has no native ext4 driver; its Linux subsystem (WSL 2) can mount an ext4 disk inside the Linux VM with `wsl --mount`. Linux has read-write ext4 in the kernel with `e2fsprogs` for format and repair. Impossible OS recognises ext4 and plans a native in-kernel read-write driver, which Windows does not have.

## See also

- [ext4 roadmap](../../todo/05-storage-filesystems/TODO-09-ext4-readwrite.md)
- [Btrfs](btrfs.md)
- [Volume Management and Auto-mount](volume-management.md)
- [ext4 specification notes](../../specs/storage/filesystems/ext4.md)
- [Storage](index.md)
