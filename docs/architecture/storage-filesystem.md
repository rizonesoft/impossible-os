# Storage & Filesystem

> **Design Decision:** Impossible OS uses **Windows-style drive letters** (`C:\`, `D:\`)
> and a custom filesystem called **IXFS** (Impossible X FileSystem) for the root partition.
> FAT32 is used only for the EFI System Partition (required by UEFI).

## Architecture Overview

```
User / Kernel code
        │
        ▼
  ┌──────────────┐
  │     VFS      │  Unified API: open, read, write, readdir
  │   (vfs.c)    │  Drive letter routing: C:\, A:\, D:\
  └──┬───┬───┘
     │   │
     ▼   ▼
  ┌─────┐ ┌─────┐
  │IXFS │ │FAT32│   Filesystem drivers (pluggable vfs_ops)
  └──┬──┘ └──┬──┘
     │       │
     ▼       ▼
  ┌──────────────────────────────┐
  │  Block Device Abstraction Layer  │  blkdev_read() / blkdev_write()
  │          (blkdev.c)              │  up to 16 registered devices
  └──┬──────────┬─────────┬──────┘
     │          │         │
     ▼          ▼         ▼
  ┌────────┐  ┌───────┐  ┌───────┐
  │VirtIO  │  │ AHCI  │  │ ATA   │   Hardware drivers
  │(MMIO)  │  │(DMA)  │  │(PIO)  │
  └────────┘  └───────┘  └───────┘
   virtio0     sata0      ata0
```

## Drive Letter Assignment

| Letter | Filesystem | Partition | Purpose |
|--------|-----------|-----------|---------|
| `A:\` | FAT32 | EFI System Partition | UEFI boot files (read-only) |
| `C:\` | IXFS | System partition | OS files, user data, programs |
| `D:\`–`Z:\` | Any | Additional drives | USB, extra disks |

At boot, `partition_scan_all()` probes all block devices and
`partition_mount_filesystems()` auto-mounts detected volumes.

## Partitioning

The kernel currently supports the **MBR (Master Boot Record)** partition table format.
During the boot sequence, right after initializing the block devices (VirtIO, AHCI, ATA), the kernel scans the first sector of each device.

### Supported Partition Types

| Type ID | Human Readable | Purpose |
|---------|----------------|---------|
| `0x0C`  | FAT32 (LBA)    | Supported for UEFI boot partitions |
| `0x0B`  | FAT32 (CHS)    | Legacy FAT32 (treated identically to 0x0C) |
| `0x83`  | Linux          | Recognized, but not mountable natively yet |
| `0xDA`  | IXFS           | Custom type ID for Native Impossible OS filesystem partitions |
| `0xEE`  | GPT Protective | Recognized (GPT parsing not yet implemented) |

The parsing implementation skips empty (`0x00`) or zero-size entries. Active partitions are logged during boot to the serial output, for example:
```text
[MBR] virtio0: 3 partition(s)
  p1: FAT32 (LBA)  LBA 2048  16 MiB [boot]
```

### GPT (GUID Partition Table)

When the MBR scan detects a protective partition (type `0xEE`), the kernel reads the GPT header at LBA 1 and validates it using CRC32 checksums. It then parses the 128-byte partition entries starting at LBA 2.

**Validation steps:**
1. Verify `"EFI PART"` signature in header
2. CRC32 check of the header (with CRC field zeroed)
3. CRC32 check of the entire partition entry array

#### Recognized Partition Type GUIDs

| GUID | Human Readable | Purpose |
|------|----------------|---------|
| `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | EFI System | UEFI boot partition |
| `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | Basic Data | Windows/general data |
| `0FC63DAF-8483-4772-8E79-3D69D8477DE4` | Linux | Linux filesystem |
| `DA000000-0000-4978-4653-000000000001` | IXFS | Custom Impossible OS partition |

#### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/gpt.c` | GPT parser with CRC32 validation |
| `include/kernel/fs/gpt.h` | GPT structures, GUIDs, API |
| `tools/make-gpt.c` | Host tool to generate GPT test images |

Example boot output:
```text
[GPT] virtio0: 3 partition(s)
  p1: EFI System  LBA 2048–4095  1 MiB  "EFI System"
  p2: Basic Data  LBA 4096–36863  16 MiB  "Windows Data"
  p3: IXFS  LBA 36864–102399  32 MiB  "Impossible OS"
```

### Partition Scanner

At boot, `partition_scan_all()` iterates over every registered block device and:

1. Reads sector 0, tries GPT (if protective MBR `0xEE` detected), otherwise falls back to MBR
2. Creates **sub-block-devices** (e.g., `disk0p1`, `disk0p2`) that offset all LBA reads/writes by the partition's start LBA
3. Probes each partition for known filesystem signatures:
   - **FAT32**: Boot signature `0x55AA` + `"FAT32"` at BPB offset 82
   - **IXFS**: Magic `0x49584653` at superblock offset 0
   - **ext2**: Magic `0xEF53` at superblock byte 1080

| File | Purpose |
|------|---------|
| `src/kernel/fs/partition.c` | Unified scanner + sub-blkdev layer |
| `include/kernel/fs/partition.h` | API and partition info struct |

Example boot output:
```text
[GPT] virtio0: 3 partition(s)
  Disk 0, Partition 1: Unknown, 1 MiB (EFI System)
  Disk 0, Partition 2: Unknown, 16 MiB (Basic Data)
  Disk 0, Partition 3: Unknown, 32 MiB (IXFS)
```


## ATA/IDE Disk Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/ata.c` | ATA PIO disk driver |
| `include/kernel/drivers/ata.h` | ATA API |

### Configuration

| Property | Value |
|----------|-------|
| Bus | Primary ATA (ports `0x1F0`–`0x1F7`, control `0x3F6`) |
| Mode | PIO (Programmed I/O) — no DMA |
| Addressing | LBA28 (supports up to 128 GiB) |
| Sector size | 512 bytes |

### API

| Function | Description |
|----------|-------------|
| `ata_init()` | Detect drives via IDENTIFY command |
| `ata_read_sectors(lba, count, buf)` | Read sectors from disk |
| `ata_write_sectors(lba, count, buf)` | Write sectors + cache flush |

## VirtIO Block Driver (Modern 1.0)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/virtio_blk.c` | VirtIO block driver (modern MMIO) |
| `include/kernel/drivers/virtio_blk.h` | VirtIO-blk API and request types |
| `src/kernel/drivers/virtio.c` | Shared modern PCI transport layer |
| `include/kernel/drivers/virtio.h` | VirtIO structs, virtqueue API |

### Configuration

| Property | Value |
|----------|-------|
| Transport | Modern VirtIO 1.0 PCI (MMIO via PCI capabilities) |
| PCI ID | Vendor `0x1AF4`, Device `0x1001` (transitional) or `0x1042` (modern) |
| Queue type | Split virtqueue (desc/avail/used rings) |
| Features | `VIRTIO_F_VERSION_1` negotiated |
| Sector size | 512 bytes |
| QEMU flag | `-drive file=disk.img,format=raw,if=none,id=disk0 -device virtio-blk-pci,drive=disk0` |

### API

| Function | Description |
|----------|-------------|
| `virtio_blk_init()` | Detect + initialize via PCI capability walking |
| `virtio_blk_read(lba, count, buf)` | Read sectors (3-descriptor chain) |
| `virtio_blk_write(lba, count, buf)` | Write sectors |
| `virtio_blk_capacity()` | Total sectors from device_cfg MMIO |
| `virtio_blk_present()` | Check if device was initialized |

## AHCI (SATA) Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/ahci.c` | AHCI SATA driver (MMIO, DMA) |
| `include/kernel/drivers/ahci.h` | AHCI register defs, HBA structs, API |

### Configuration

| Property | Value |
|----------|-------|
| Transport | AHCI MMIO via PCI BAR5 (ABAR) |
| PCI detection | Class `0x01` (Storage), Subclass `0x06` (AHCI) |
| Controller | Intel ICH9 AHCI (QEMU default) |
| Command model | DMA via command list + PRDT entries |
| Addressing | LBA48 (up to 128 PiB) |
| Sector size | 512 bytes |
| QEMU flags | `-drive file=sata.img,format=raw,if=none,id=disk1 -device ahci,id=ahci0 -device ide-hd,drive=disk1,bus=ahci0.0` |

### API

| Function | Description |
|----------|-------------|
| `ahci_init()` | Detect controller, init HBA, enumerate ports, IDENTIFY |
| `ahci_read(port, lba, count, buf)` | Read sectors via READ DMA EXT |
| `ahci_write(port, lba, count, buf)` | Write sectors via WRITE DMA EXT |
| `ahci_identify(port)` | Get model, serial, capacity |
| `ahci_capacity(port)` | Total sectors for given port |
| `ahci_present()` | Check if any SATA/ATAPI device initialized |
| `ahci_drive_count()` | Number of detected SATA drives |
| `ahci_atapi_count()` | Number of detected ATAPI (optical) devices |
| `ahci_atapi_read(idx, lba, count, buf)` | Read 2048-byte sectors via SCSI READ(10) |
| `ahci_atapi_capacity(idx)` | Total 2048-byte sectors on disc |
| `ahci_atapi_sector_size(idx)` | Sector size (typically 2048) |

### ATAPI (Optical Disc) Support

ATAPI devices (CD/DVD/Blu-ray drives) appear on AHCI ports with signature
`0xEB140101` (vs `0x00000101` for SATA disks). The driver detects this during
port enumeration and uses SCSI packet commands instead of ATA commands.

| Property | Value |
|----------|-------|
| Detection | AHCI port signature `0xEB140101` |
| Identify | `ATA_CMD_IDENTIFY_PACKET` (`0xA1`) |
| Command model | SCSI CDBs via `ATA_CMD_PACKET` (`0xA0`), using `acmd[16]` in command table |
| Sector size | 2048 bytes (standard optical media) |
| Read-only | Write callback is NULL |
| blkdev name | `cdrom0`, `cdrom1`, etc. |

**SCSI Commands Used:**

| Command | Opcode | Purpose |
|---------|--------|---------|
| TEST UNIT READY | `0x00` | Check if disc is present |
| INQUIRY | `0x12` | Device identification |
| READ CAPACITY (10) | `0x25` | Get disc size and block size |
| READ (10) | `0x28` | Read sectors from disc |

**QEMU flags:**
```
-drive id=cdrom0,file=test.iso,format=raw,if=none,media=cdrom
-device ide-cd,drive=cdrom0,bus=ahci0.1
```

## Block Device Abstraction Layer

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/blkdev.c` | Device registry, dispatch, listing |
| `include/kernel/drivers/blkdev.h` | `struct blkdev` and API |

### Configuration

| Property | Value |
|----------|-------|
| Max devices | 16 (BLKDEV_MAX) |
| Dispatch | Function-pointer based |
| Naming | "virtio0", "sata0", "sata1", etc. |

### API

| Function | Description |
|----------|-------------|
| `blkdev_register(dev)` | Add device to global registry |
| `blkdev_get(name)` | Look up by name string |
| `blkdev_get_by_index(idx)` | Look up by index |
| `blkdev_read(dev, lba, count, buf)` | Dispatch read to driver |
| `blkdev_write(dev, lba, count, buf)` | Dispatch write to driver |
| `blkdev_count()` | Number of registered devices |
| `blkdev_list()` | Print all devices to serial/console |

## Virtual Filesystem (VFS)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/vfs.c` | VFS layer with drive letter routing |
| `include/kernel/fs/vfs.h` | VFS node, dirent, and API |

### VFS Node Structure

```c
struct vfs_node {
    char     name[256];
    uint32_t type;       /* VFS_FILE, VFS_DIRECTORY */
    uint32_t size;
    uint32_t inode;
    /* Function pointers — filled by each FS driver */
    read_fn  read;
    write_fn write;
    open_fn  open;
    close_fn close;
    readdir_fn readdir;
    finddir_fn finddir;
};
```

### Path Resolution

Paths use **Windows-style backslash notation**:

```
C:\Users\Default\readme.txt
│  └────────────────────────── Path within filesystem
└──────────────────────────── Drive letter
```

The VFS parses the drive letter, looks up the mounted filesystem, and delegates
to the appropriate driver's operations.

### API

| Function | Description |
|----------|-------------|
| `vfs_open(node)` | Open a file node |
| `vfs_read(node, offset, size, buf)` | Read file data |
| `vfs_write(node, offset, size, buf)` | Write file data |
| `vfs_close(node)` | Close a file node |
| `vfs_readdir(node, index)` | List directory entry at index |
| `vfs_finddir(node, name)` | Find child by name |

## Initial RAM Filesystem (initrd) — Removed

> **Removed** in commit `56c2a12`. The initrd subsystem (`initrd.c`,
> `initrd.h`, `make-initrd.c`) has been fully deleted. All system files now
> live on the IXFS partition (`C:\`), pre-populated at build time by
> `mkfs-ixfs --populate build/sysroot`. Syscalls `SYS_READFILE`,
> `SYS_READDIR`, and `SYS_EXEC` use `vfs_get_drive_root('C')` instead.

### Boot Flow (Current — Disk Boot)

```
OVMF loads GRUB from EFI System Partition
  ↓  GRUB loads kernel.exe via multiboot2
Kernel starts — VFS init
  ↓  PCI/VirtIO/AHCI drivers init
partition_scan_all() → IXFS detected on system-disk.img
  ↓
IXFS auto-mounted at C:\ (pre-populated with all files)
  ↓
Desktop loads wallpaper, icons from C:\ via VFS
Shell/exec loaders read PEs from C:\
```

## FAT32 Filesystem Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/fat32.c` | FAT32 read/write driver |
| `include/kernel/fs/fat32.h` | FAT32 API |

### Capabilities

| Feature | Status |
|---------|--------|
| BPB parsing | ✅ |
| FAT chain following | ✅ |
| Directory listing | ✅ (8.3 short names + LFN) |
| Long filenames (LFN) | ✅ (UTF-16LE assembly) |
| File reading | ✅ |
| `fat32_stat(path)` | ✅ (size, attributes, timestamps) |
| Block device layer | ✅ (via `blkdev_read/write`, works with partition sub-devices) |
| File writing | ✅ (create, write, delete, rename) |
| Directory creation | ✅ (with `.` and `..` entries) |
| `fat32_format()` | ✅ (BPB, FSInfo, dual FAT, root dir) |
| FAT flush | ✅ (writes both FAT copies on every modification) |

The FAT32 driver supports full read/write operations via the `blkdev`
abstraction layer (using partition scanner sub-blkdevs) and supports
both 8.3 short names and LFN (Long File Name) entries.

### VFS Integration

FAT32 partitions are automatically mounted at boot by
`partition_mount_filesystems()`, starting at drive letter `D:\`.
All VFS operations are supported:

- **open/close** — no-op (FAT32 has no file locks)
- **read** — cluster chain traversal with offset seeking
- **write** — full-file overwrite via cluster reallocation
- **readdir/finddir** — directory scanning with LFN assembly
- **create** — allocates first cluster, writes directory entry (files and dirs)
- **unlink** — frees cluster chain, marks entry as `0xE5`

## IXFS — Impossible X FileSystem

> Full on-disk format specification: [`ixfs-specification.md`](ixfs-specification.md)

IXFS is the native Impossible OS filesystem, mounted at `C:\` (system drive).

| Property | Value |
|----------|-------|
| Block size | 4 KiB (matches page size) |
| Inode size | 128 bytes (32 per block) |
| Max file size | ~4 GiB (12 direct + indirect + double-indirect) |
| Max volume | 32 GiB (current block group limit) |
| Allocation | Bitmap + block group allocator (locality-aware) |
| I/O layer | `blkdev_read/write` (works with any storage backend) |

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/ixfs/` | IXFS kernel driver (core, ops, alloc, format, etc.) |
| `include/kernel/fs/ixfs.h` | On-disk structures and public API |
| `docs/architecture/ixfs-specification.md` | Full on-disk format specification |

### Implemented Features

- **Full CRUD**: create, read, write, delete, rename files and directories
- **Block device abstraction**: all I/O via `blkdev` (partition sub-devices)
- **Block group allocator**: 32768 blocks/group, locality hints, O(1) sequential alloc
- **Auto-mount**: `partition_mount_filesystems()` detects IXFS and mounts at `C:\`
- **Permissions**: Unix rwx, uid/gid, `ixfs_check_perm()`
- **Timestamps**: created, modified, accessed
- **Pre-populated disk**: `mkfs-ixfs --populate` creates ready-to-boot IXFS images

### Planned Enhancements

See `TODO-Phase-06.md` §5.5–5.11 for the full roadmap:
extent-based allocation, journaling, CoW/snapshots, checksums, 64-bit addressing.
