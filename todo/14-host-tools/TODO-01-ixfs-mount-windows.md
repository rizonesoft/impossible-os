# TODO-01 — IXFS Mount for Windows (WinFsp)

> **Goal:** Mount IXFS partitions as Windows drive letters using WinFsp, so developers can browse `C:\Impossible\` from Windows Explorer during development. Read logs, inspect files, copy assets — without booting the OS.

> [!IMPORTANT]
> This is a **host-side development tool**, not part of the Impossible OS kernel. It runs on Windows, uses the native Windows compiler (MSVC or MinGW), and depends on WinFsp. It reads the same IXFS on-disk structures as the kernel's `src/kernel/fs/ixfs/` but through Windows file handles instead of AHCI sectors.

> [!NOTE]
> **WinFsp** (Windows File System Proxy) provides a FUSE-like API for Windows. You implement filesystem callbacks (Open, Read, Write, ReadDirectory), WinFsp handles drive letter mounting, Explorer integration, caching, and security. No kernel driver, no code signing, no WHQL. Free and open source (GPLv3 with FLOSS exception).

## Inputs

- [`src/kernel/fs/ixfs/ixfs_core.c`](../../src/kernel/fs/ixfs/ixfs_core.c) — IXFS superblock, mount, metadata
- [`src/kernel/fs/ixfs/ixfs_inode.c`](../../src/kernel/fs/ixfs/ixfs_inode.c) — inode read/write
- [`src/kernel/fs/ixfs/ixfs_extent.c`](../../src/kernel/fs/ixfs/ixfs_extent.c) — extent-based block mapping
- [`src/kernel/fs/ixfs/ixfs_ops.c`](../../src/kernel/fs/ixfs/ixfs_ops.c) — file operations (read, write, readdir)
- [`src/kernel/fs/ixfs/ixfs_alloc.c`](../../src/kernel/fs/ixfs/ixfs_alloc.c) — block allocator
- [`include/kernel/fs/ixfs.h`](../../include/kernel/fs/ixfs.h) — on-disk structures (superblock, inode, extent)
- [`tools/mkfs-ixfs.c`](../../tools/mkfs-ixfs.c) — IXFS formatter (reference for on-disk layout)

## Outcome

- `ixfs-mount.exe I: \\.\PhysicalDrive0 3` — mount partition 3 of disk 0 as `I:\`
- `ixfs-mount.exe I: build\system-disk.img 3` — mount partition 3 of a disk image file
- Windows Explorer shows IXFS contents: files, directories, sizes, timestamps
- Read and write support (write requires unmount or flush to avoid corruption)
- `ixfs-unmount.exe I:` — clean unmount

## Source Layout

```
sdk/src/ixfs-mount/
  ├── ixfs-mount.c          # WinFsp filesystem callbacks + main
  ├── ixfs-disk.c           # Raw disk/image I/O (CreateFile + ReadFile/WriteFile)
  ├── ixfs-disk.h           # Disk I/O API
  ├── ixfs-structs.h        # On-disk structures (copied from include/kernel/fs/ixfs.h)
  ├── Makefile              # Build with MSVC or MinGW (NOT clang-19 cross)
  └── README.md             # Usage, WinFsp install instructions
```

> [!NOTE]
> `ixfs-structs.h` is a COPY of the on-disk structures from the kernel headers, adapted for Windows types (`uint32_t` from `<stdint.h>` instead of kernel types). Keep in sync manually — any on-disk format change in the kernel must be reflected here.

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Raw disk/image I/O layer                       | —          |  [ ]   |
| 💎  |   2   | IXFS superblock + inode parsing                | §1         |  [ ]   |
| 💎  |   3   | WinFsp read-only mount (browse + read files)   | §2         |  [ ]   |
| ⭐  |   4   | Write support (create, write, delete)          | §3         |  [ ]   |
| 💎  |   5   | Disk image mode (mount .img files directly)    | §3         |  [ ]   |

---

## 1. Raw Disk/Image I/O Layer
Read and write sectors from a physical disk partition or a raw disk image file on Windows.

**Files:** `sdk/src/ixfs-mount/ixfs-disk.c`, `ixfs-disk.h`

- [ ] `disk_open(path, partition_index)` — open physical disk (`\\.\PhysicalDrive0`) or image file, seek to partition start via GPT parsing
- [ ] `disk_read_sectors(lba, count, buf)` — read sectors at LBA offset within the partition
- [ ] `disk_write_sectors(lba, count, buf)` — write sectors (for write support)
- [ ] `disk_close()` — flush and close
- [ ] GPT partition table parser: find partition by index, get start LBA and size
- [ ] Support both `\\.\PhysicalDrive0` (physical) and `path\to\image.img` (file)
- [ ] Commit: `"tools: ixfs-mount raw disk I/O layer for Windows"`

**Test checkpoint:** Read sector 0 of IXFS partition from `build\system-disk.img`, verify IXFS magic.

## 2. IXFS Superblock + Inode Parsing
Parse the IXFS on-disk structures: superblock, inode table, extent maps. Port the relevant logic from kernel IXFS code.

**Files:** `sdk/src/ixfs-mount/ixfs-mount.c`, `ixfs-structs.h`

- [ ] Copy on-disk structures from `include/kernel/fs/ixfs.h` to `ixfs-structs.h` (adapted for Windows `<stdint.h>`)
- [ ] `ixfs_read_superblock()` — read and validate superblock (magic, version, block size)
- [ ] `ixfs_read_inode(ino)` — read inode from inode table
- [ ] `ixfs_read_block(blk, buf)` — read a data block
- [ ] `ixfs_extent_lookup(inode, file_block)` — resolve file block to disk block via extent list
- [ ] `ixfs_readdir(inode)` — enumerate directory entries
- [ ] Commit: `"tools: ixfs-mount IXFS parser — superblock, inodes, extents, readdir"`

**Test checkpoint:** Parse `build\system-disk.img` partition 3, list root directory entries.

## 3. WinFsp Read-Only Mount
Implement WinFsp filesystem callbacks for read-only browsing. Mount IXFS as a Windows drive letter.

**Files:** `sdk/src/ixfs-mount/ixfs-mount.c`

- [ ] WinFsp `GetVolumeInfo` — return volume label, total/free space from superblock
- [ ] WinFsp `GetSecurityByName` — return basic security descriptor (everyone read)
- [ ] WinFsp `Open` — look up file/directory by path, return inode handle
- [ ] WinFsp `Read` — read file data via extent lookup
- [ ] WinFsp `ReadDirectory` — enumerate directory entries
- [ ] WinFsp `GetFileInfo` — return size, timestamps, attributes from inode
- [ ] WinFsp `Close` — release handle
- [ ] Command-line: `ixfs-mount.exe I: \\.\PhysicalDrive0 3`
- [ ] Commit: `"tools: ixfs-mount WinFsp read-only mount — browse IXFS from Explorer"`

**Test checkpoint:** `ixfs-mount.exe I: build\system-disk.img 3` — open `I:\` in Explorer, see Impossible OS files, open `boot.conf`, read log files.

## 4. Write Support
Add create, write, delete, and rename operations for full read-write access.

**Files:** `sdk/src/ixfs-mount/ixfs-mount.c`

- [ ] WinFsp `Create` — allocate inode, add directory entry
- [ ] WinFsp `Write` — allocate blocks, update extents, write data
- [ ] WinFsp `SetFileInfo` — update timestamps, attributes
- [ ] WinFsp `Cleanup` / `Delete` — remove files, free blocks
- [ ] WinFsp `Rename` — move/rename files and directories
- [ ] Block allocator: port from `ixfs_alloc.c` (bitmap-based)
- [ ] Flush on unmount: write dirty inodes + superblock
- [ ] Commit: `"tools: ixfs-mount read-write support — full Explorer integration"`

**Test checkpoint:** Create a file in `I:\test.txt` from Explorer, unmount, boot Impossible OS, verify file exists on `C:\`.

## 5. Disk Image Mode
Mount raw `.img` files directly without needing a physical disk. Useful for development with QEMU disk images.

**Files:** `sdk/src/ixfs-mount/ixfs-disk.c`

- [ ] Auto-detect: if path doesn't start with `\\.\`, treat as image file
- [ ] Image file locking: open with `FILE_SHARE_READ` to prevent QEMU conflicts
- [ ] Support concurrent QEMU access: read-only mode when QEMU is running, read-write when not
- [ ] Commit: `"tools: ixfs-mount disk image mode — mount build/system-disk.img directly"`

**Test checkpoint:** `ixfs-mount.exe I: build\system-disk.img 3` — browse files while QEMU is not running.

---

## Build Instructions

```bash
# Prerequisites: install WinFsp from https://winfsp.dev/
# WinFsp headers/libs at: C:\Program Files (x86)\WinFsp\

# Build with MSVC (Developer Command Prompt):
cd sdk\src\ixfs-mount
cl /O2 /I"C:\Program Files (x86)\WinFsp\inc" ixfs-mount.c ixfs-disk.c \
   /link /LIBPATH:"C:\Program Files (x86)\WinFsp\lib" winfsp-x64.lib \
   /OUT:..\..\tools\ixfs-mount.exe

# Build with MinGW:
gcc -O2 -I"/c/Program Files (x86)/WinFsp/inc" ixfs-mount.c ixfs-disk.c \
    -L"/c/Program Files (x86)/WinFsp/lib" -lwinfsp-x64 -o ../../tools/ixfs-mount.exe
```

> [!NOTE]
> SDK tools build independently from the kernel. Source in `sdk/src/`, binaries output to `sdk/tools/`. Uses native Windows compiler + WinFsp SDK, not the kernel's clang-19 cross-compiler.

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | Cross-OS filesystem     | ✅ ext2fsd, Linux FS         | ✅ ntfs-3g, mount.cifs       | ⬜ §3 — IXFS on Windows         |
| ⭐ | Dev workflow mount      | ❌ No NTFS-on-Linux dev tool | ❌ No ext4-on-Windows dev    | ⬜ §5 — mount disk images       |

## Verification

- [ ] `ixfs-mount.exe I: build\system-disk.img 3` — Explorer shows IXFS contents
- [ ] Read: open log files, view boot.conf, copy wallpaper to desktop
- [ ] Write: create test file, verify in Impossible OS after boot
- [ ] Image mode: works with `build\system-disk.img` from WSL path
