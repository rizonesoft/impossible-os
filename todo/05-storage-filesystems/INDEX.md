# 05 Storage Filesystems

This domain owns the storage stack after hardware controllers: block abstraction, partition tables, mounts, VFS behavior, filesystem drivers, and file API semantics.

## Belongs Here

- Block-device adapters, partition tables, mount flow, volume discovery, and drive-letter policy.
- VFS work, on-disk filesystem formats, and concrete filesystem implementations such as FAT32, IXFS, and NTFS.
- Win32 file API semantics, path behavior, handle semantics, and filesystem-facing compatibility work.

## Does Not Belong Here

- Controller-specific driver work such as AHCI, VirtIO, NVMe, or USB transport. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).
- Disk GUI apps and user-facing tools. Put that in [10 Apps](../11-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/fs](../src/kernel/fs/)
- [src/kernel/main/blkdev_adapters.c](../src/kernel/main/blkdev_adapters.c)
- [include/kernel/fs](../include/kernel/fs/)

## Epics

- None yet.

## Active TODOs

- [`TODO-01-block-storage-hardening.md`](TODO-01-block-storage-hardening.md) -- VirtIO-blk flush/retry, AHCI NCQ/SMART/error recovery, per-device I/O metrics, LRU sector cache
- [`TODO-02-ntfs-readwrite.md`](TODO-02-ntfs-readwrite.md) -- NTFS B+ tree mutation, write/truncate path wiring, volume formatter, $Secure, dirty recovery
- [`TODO-03-volume-management-automount.md`](TODO-03-volume-management-automount.md) -- Filesystem probe chain, dynamic drive-letter assignment, USB hot-plug, safe removal, Win32 volume APIs, FSCTL ioctls
- [`TODO-04-fat32-hardening-vfs-semantics.md`](TODO-04-fat32-hardening-vfs-semantics.md) -- FAT32 BPB validation, LFN write, dual-FAT repair, fsck, timestamps; VFS share-mode, delete-on-close, byte-range locks, Win32 feature stubs
- [`TODO-05-win32-file-io-api.md`](TODO-05-win32-file-io-api.md) -- IRP engine, MDL, file handle table, CreateFile/ReadFile/WriteFile, directories, async I/O, filter manager, internal migration -- **P0 blocker for all user-mode programs**
- [`TODO-06-ixfs-core-win32-compat.md`](TODO-06-ixfs-core-win32-compat.md) -- IXFS v3 inode upgrade, subsystem verification, case-insensitive paths, ADS, security descriptors, hard links, symlinks, EAs, object IDs, v3 format tool -- **P0 blocker for Win32 apps on system volume**
- [`TODO-07-ixfs-advanced-enterprise.md`](TODO-07-ixfs-advanced-enterprise.md) -- IXFS compression, inline dedup, reflinks, online defrag, sparse, TRIM, online resize, self-healing, auto-snapshots, per-file encryption, USN journal, quotas, storage tiering, health dashboard
- [`TODO-08-exfat-readwrite.md`](TODO-08-exfat-readwrite.md) -- Full exFAT R/W driver: VBR + boot checksum, Allocation Bitmap, Up-Case Table, directory entry sets, file read (FAT-chain + NoFatChain), file write, create/delete/rename, timestamps, VFS probe registration, Unicode edge cases, large files > 4 GiB
- [`TODO-09-ext4-readwrite.md`](TODO-09-ext4-readwrite.md) -- Full ext4 R/W driver: superblock + block group descriptors, inode reader, extent tree decoder, directory htree, file read (inline data + extents), JBD2 journal replay, file write, extent tree mutation, directory write, create/delete/rename, xattrs, VFS registration -- **⭐ exclusive: Windows 11 cannot read ext4**
- [`TODO-10-btrfs-readonly.md`](TODO-10-btrfs-readonly.md) -- Read-only Btrfs driver: superblock CRC32C, B-tree node format, chunk tree logical→physical, tree search + walk, root tree + subvolume enumeration, inode reader, extent data with zlib/LZO/Zstd decompression, directory reader, symlinks + xattrs, VFS registration -- **⭐ exclusive: no other non-Linux OS reads Btrfs natively**
- [`TODO-11-optical-media.md`](TODO-11-optical-media.md) -- Optical media: SCSI MMC (READ TOC/DISC INFO/TRACK INFO), ISO 9660 base, Rock Ridge SUSP, Joliet UCS-2BE, UDF AVDP→ICB, auto-probe UDF > Joliet > ISO 9660, `GetVolumeInformationW`, audio CD raw READ CD -- **⭐ Rock Ridge exclusive over Windows 11**
- [`TODO-12-apple-filesystems-readonly.md`](TODO-12-apple-filesystems-readonly.md) -- Read-only APFS + HFS+ drivers: APFS container/omap B-tree/volume/fstree/inodes/extents/dirs/xattrs; HFS+ header/journal replay/Catalog B-tree/extents overflow; both GPT GUID guarded -- **⭐ exclusive: only non-Apple OS with native in-kernel APFS read**
- [`TODO-13-partition-tools-storage-suite.md`](TODO-13-partition-tools-storage-suite.md) -- GPT + MBR partition write APIs, `diskpart` CLI, `chkdsk`/`defrag`/`sfc`/`recover`/`diskuse`/`snapshot` CLI+GUI, Disk Management GUI -- **⭐ built-in file recovery + treemap diskuse + IXFS snapshot diff viewer exceed Windows 11**
- [`TODO-14-disk-benchmark-diagnostics.md`](TODO-14-disk-benchmark-diagnostics.md) -- `diskbench` CLI + GUI (seq/rand/mixed MB/s + IOPS + p99 latency + QD sweep), SMART health score, generic `blkdev_stats_t` histogram API, `/sys/ioqueue`, VFS hot-path profiler, storage health monitor daemon -- **⭐ exclusive: no other default OS install ships a built-in disk benchmark or VFS profiler**

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-vfs-core.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
