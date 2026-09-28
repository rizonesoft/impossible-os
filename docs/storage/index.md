# Storage

Block storage, drive letters, filesystems and disk tools: the block-device layer and its hardening, volume management, FAT32 and the VFS, the Win32 file calls, IXFS, NTFS, exFAT, ext4, Btrfs, optical discs, Apple filesystems, partition tools and disk diagnostics. The disk controllers themselves (AHCI, NVMe, VirtIO-blk, USB mass storage) are covered in [Storage Controllers and Removable Media](../hardware/storage-controllers.md).

## Roadmap Overviews

One page per storage and filesystems roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Block Storage Hardening](block-storage-hardening.md) | Block-device pass-through, VirtIO-blk flush and retry, AHCI NCQ and recovery, planned SMART, counters and cache |
| [NTFS](ntfs.md) | Read-only mounts today, the unwired write engine and journal, planned format and replay |
| [Volume Management and Auto-mount](volume-management.md) | Boot probe order, drive-letter rules, planned hot-plug, safe removal and volume APIs |
| [FAT32 and VFS Semantics](fat32-vfs.md) | Strict mount checks, auto-repair, long names, share modes, locks and oplocks |
| [Win32 File I/O](win32-file-io.md) | NT file calls on the VFS today, planned IRP engine, async I/O and `kernel32` |
| [IXFS Core](ixfs-core.md) | Version 2 on-disk layout, `C:` mount, case folding, planned v3 inode and Windows file features |
| [IXFS Advanced Storage](ixfs-advanced.md) | Journal, snapshots, scrub and repair pass, planned compression, dedup, encryption and quotas |
| [exFAT](exfat.md) | Why exFAT media do not mount, planned clean-room driver |
| [ext4](ext4.md) | ext magic detection, licence constraint, planned read-write driver with JBD2 replay |
| [Btrfs](btrfs.md) | Planned read-only driver: chunk map, subvolumes, compressed extents |
| [Optical Media](optical-media.md) | Working ATAPI layer and `cdromN` devices, planned ISO 9660, Joliet and UDF |
| [Apple Filesystems (APFS and HFS+)](apple-filesystems.md) | Partition type recognition, planned read-only APFS and HFS+ |
| [Partition Management and Storage Tools](partition-tools.md) | Partition parsing, formatters and repair engines, planned `diskpart`, `chkdsk` and Disk Management |
| [Disk Benchmark and I/O Diagnostics](disk-diagnostics.md) | VirtIO-blk latency histogram, planned statistics, `diskbench` and SMART health |

## Specifications

Reference specifications live outside the docs tree, grouped by area:

| Area | Scope |
| --- | --- |
| [Storage Controllers](../../specs/storage/controllers/) | AHCI, VirtIO, ATAPI and SCSI MMC, NVMe |
| [Partitioning](../../specs/storage/partitioning/) | GPT and MBR partition tables |
| [Filesystems](../../specs/storage/filesystems/) | FAT32, exFAT, NTFS, ext4, Btrfs, ISO 9660, Joliet and UDF, APFS, HFS+ |
