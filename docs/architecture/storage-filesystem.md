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
  ┌──────────────┐
  │  ATA Driver  │  ata_read_sectors() / ata_write_sectors()
  │   (ata.c)    │  PIO mode, LBA28
  └──────────────┘
```

## Drive Letter Assignment

| Letter | Filesystem | Partition | Purpose |
|--------|-----------|-----------|---------|
| `A:\` | FAT32 | EFI System Partition | UEFI boot files (read-only) |
| `C:\` | IXFS | Root partition | OS files, user data |
| `D:\`–`Z:\` | Any | Additional drives | USB, extra disks |

During early boot (before disk drivers), the **initrd** is temporarily mounted
at `C:\` to provide the kernel with essential files.

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
| Directory listing | ✅ (8.3 short names) |
| File reading | ✅ |
| File writing | ❌ (read-only) |
| Long filenames (LFN) | ❌ |

The FAT32 driver is intentionally read-only — it only needs to read
the EFI System Partition (`A:\`). Write support is unnecessary since the
ESP is managed by the installer/GRUB, not the running OS.

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
