# 05 Storage Filesystems

This domain owns the storage stack after hardware controllers: block abstraction, partition tables, mounts, VFS behavior, filesystem drivers, and file API semantics.

## Belongs Here

- Block-device adapters, partition tables, mount flow, volume discovery, and drive-letter policy.
- VFS work, on-disk filesystem formats, and concrete filesystem implementations such as FAT32, IXFS, and NTFS.
- Win32 file API semantics, path behavior, handle semantics, and filesystem-facing compatibility work.

## Does Not Belong Here

- Controller-specific driver work such as AHCI, VirtIO, NVMe, or USB transport. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).
- Disk GUI apps and user-facing tools. Put that in [10 Apps](../10-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/fs](../src/kernel/fs/)
- [src/kernel/main/blkdev_adapters.c](../src/kernel/main/blkdev_adapters.c)
- [include/kernel/fs](../include/kernel/fs/)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-vfs-core.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
