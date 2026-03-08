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
  └──┬───┬───┬───┘
     │   │   │
     ▼   ▼   ▼
  ┌─────┐ ┌─────┐ ┌─────┐
  │IXFS │ │FAT32│ │initrd│   Filesystem drivers (pluggable vfs_ops)
  └──┬──┘ └──┬──┘ └─────┘
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
| `C:\` | IXFS | Root partition | OS files, user data |
| `D:\`–`Z:\` | Any | Additional drives | USB, extra disks |

During early boot (before disk drivers), the **initrd** is temporarily mounted
at `C:\` to provide the kernel with essential files.

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
| `ahci_present()` | Check if any SATA drive initialized |
| `ahci_drive_count()` | Number of detected SATA drives |

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

## Initial RAM Filesystem (initrd)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/initrd.c` | Initrd driver |
| `tools/make-initrd.c` | Host tool to create initrd images |

### Format

Custom archive format with magic `IXRD`:

```
┌────────────┬───────────────┬───────────────┬─────┐
│ Header     │ File entry 0  │ File entry 1  │ ... │
│ (magic,    │ (name, offset,│               │     │
│  count)    │  size, data)  │               │     │
└────────────┴───────────────┴───────────────┴─────┘
```

### Contents (Current Build)

| File | Description |
|------|-------------|
| `hello.txt` | Test text file |
| `readme.txt` | Project readme |
| `hello.exe` | Hello world user program |
| `shell.exe` | Interactive shell |
| `wallpaper.raw` | Desktop wallpaper (RAW pixels) |
| `bg.raw` | Background gradient |
| `start_icon.raw` | Start menu icon |

The initrd is loaded by GRUB as a Multiboot2 module and mounted at `C:\`
during early boot until the real disk filesystem takes over.

## FAT32 Filesystem Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/fat32.c` | FAT32 read-only driver |
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

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/ixfs.c` | IXFS kernel driver |
| `include/kernel/fs/ixfs.h` | IXFS API and on-disk structures |
| `tools/mkfs-ixfs.c` | Host-side formatter |

### On-Disk Layout

```
Block 0:    Superblock (512 bytes)
Block 1–N:  Block bitmap (1 bit per 4 KiB block)
Block N+1:  Inode table start
Block M+1:  Data blocks start
```

### Superblock

| Field | Size | Description |
|-------|------|-------------|
| `magic` | 4 bytes | `0x49584653` ("IXFS") |
| `version` | 4 bytes | Format version |
| `block_size` | 4 bytes | 4096 (4 KiB) |
| `total_blocks` | 4 bytes | Total blocks on partition |
| `free_blocks` | 4 bytes | Free block count |
| `inode_count` | 4 bytes | Total inodes |
| `root_inode` | 4 bytes | Inode number of root directory |

### Inode (128 bytes)

| Field | Size | Description |
|-------|------|-------------|
| `type` | 2 bytes | File or directory |
| `permissions` | 2 bytes | Unix-style rwx |
| `uid` / `gid` | 4 bytes | Owner/group IDs |
| `size` | 8 bytes | File size in bytes |
| `created` | 8 bytes | Creation timestamp |
| `modified` | 8 bytes | Last modification |
| `accessed` | 8 bytes | Last access |
| `direct[12]` | 48 bytes | 12 direct block pointers |
| `indirect` | 4 bytes | Single indirect block pointer |

### Directory Entries (256 bytes each)

| Field | Size | Description |
|-------|------|-------------|
| `inode` | 4 bytes | Inode number |
| `name` | 252 bytes | Filename (up to 251 chars + null) |

16 directory entries per 4 KiB block.

### Features

| Feature | Status | Details |
|---------|--------|---------|
| Create files | ✅ | `ixfs_create()` |
| Read files | ✅ | `ixfs_read()` |
| Write files | ✅ | `ixfs_write()` |
| Delete files | ✅ | `ixfs_delete()` |
| Create directories | ✅ | `ixfs_mkdir()` |
| List directories | ✅ | `ixfs_readdir()` |
| Remove directories | ✅ | Empty check enforced |
| Long filenames | ✅ | Up to 251 characters |
| Permissions | ✅ | Unix rwx, uid/gid, `ixfs_check_perm()` |
| Timestamps | ✅ | Created, modified, accessed (via `uptime()`) |
| Auto-format | ✅ | First boot creates Windows-like folder hierarchy |

### Default Directory Hierarchy

On fresh format, IXFS creates:

```
C:\
├── Users\
│   └── Default\
├── System\
├── Programs\
└── Temp\
```
