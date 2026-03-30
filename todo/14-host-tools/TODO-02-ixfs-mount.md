# TODO-02 — IXFS Mount (Windows + Linux)

> **Goal:** Mount IXFS partitions from both Windows and Linux so developers can browse Impossible OS filesystems from the host — read logs, inspect files, copy assets, without booting the OS. Also provides mount scripts for USB boot drives.

> [!IMPORTANT]
> This is an **SDK tool**, not part of the Impossible OS kernel. Two platform builds from shared core code:
> - **Windows:** WinFsp (FUSE-like) — mounts as drive letter in Explorer
> - **Linux:** libfuse3 — mounts via standard `mount` / `fusermount`
>
> Both share the same IXFS parsing code (`ixfs-core.c`), with platform-specific I/O and FUSE backends.

## Inputs

- [`src/kernel/fs/ixfs/ixfs_core.c`](../../src/kernel/fs/ixfs/ixfs_core.c) — IXFS superblock, mount, metadata
- [`src/kernel/fs/ixfs/ixfs_inode.c`](../../src/kernel/fs/ixfs/ixfs_inode.c) — inode read/write
- [`src/kernel/fs/ixfs/ixfs_extent.c`](../../src/kernel/fs/ixfs/ixfs_extent.c) — extent-based block mapping
- [`src/kernel/fs/ixfs/ixfs_ops.c`](../../src/kernel/fs/ixfs/ixfs_ops.c) — file operations (read, write, readdir)
- [`src/kernel/fs/ixfs/ixfs_alloc.c`](../../src/kernel/fs/ixfs/ixfs_alloc.c) — block allocator
- [`include/kernel/fs/ixfs.h`](../../include/kernel/fs/ixfs.h) — on-disk structures
- [`tools/mkfs-ixfs.c`](../../tools/mkfs-ixfs.c) — IXFS formatter (reference for on-disk layout)
- → XREF: `14-host-tools/TODO-01-sdk-build-system.md §4` — SDK build system discovers and builds this tool
- → XREF: `14-host-tools/TODO-06-disk-inspect.md` — shares IXFS core parser (`ixfs-core.c`); coordinate struct changes
- → XREF: `14-host-tools/TODO-07-ixfs-fsck.md` — shares IXFS core parser; coordinate struct changes

## Outcome

**Windows:**
- `ixfs-mount.exe I: \\.\PhysicalDrive0 2` — mount partition 2 as `I:\`
- `ixfs-mount.exe I: build\system-disk.img 2` — mount from disk image
- Double-click `mount-ixfs-usb.bat` — auto-detect USB drive, mount IXFS partition

**Linux:**
- `ixfs-mount /dev/sdb2 /mnt/ixfs` — mount partition
- `ixfs-mount build/system-disk.img:2 /mnt/ixfs` — mount from disk image
- `./mount-ixfs-usb.sh` — auto-detect USB drive, mount IXFS partition

## Source Layout

```
sdk/src/ixfs-mount/
  ├── ixfs-core.c           # Shared: superblock, inode, extent, readdir parsing
  ├── ixfs-core.h           # Shared: IXFS API for FUSE backends
  ├── ixfs-structs.h        # On-disk structures (from include/kernel/fs/ixfs.h, host types)
  ├── ixfs-disk.c           # Shared: raw disk/image I/O (platform-abstracted)
  ├── ixfs-disk.h           # Disk I/O API
  ├── ixfs-fuse-win.c       # Windows: WinFsp callbacks + main
  ├── ixfs-fuse-linux.c     # Linux: libfuse3 callbacks + main
  ├── Makefile              # Linux build (gcc + libfuse3)
  └── build-win.bat         # Windows build (MSVC or MinGW + WinFsp)

sdk/scripts/
  ├── mount-ixfs-usb.bat    # Windows: auto-detect USB, mount IXFS partition
  ├── mount-ixfs-usb.ps1    # Windows: PowerShell implementation
  ├── mount-ixfs-usb.sh     # Linux: auto-detect USB, mount IXFS partition
  └── unmount-ixfs.bat      # Windows: clean unmount

sdk/tools/                  # Compiled output (gitignored)
  ├── ixfs-mount.exe        # Windows binary
  └── ixfs-mount            # Linux binary
```

> [!NOTE]
> `ixfs-structs.h` is a COPY of on-disk structures from kernel headers, using `<stdint.h>` instead of kernel types. Keep in sync manually — any IXFS format change must be reflected here.

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Shared IXFS core parser                        | —          |  [x]   |
| 💎  |   2   | Platform disk I/O layer                        | §1         |  [ ]   |
| 💎  |   3   | Linux FUSE mount (read-only)                   | §2         |  [ ]   |
| 💎  |   4   | Windows WinFsp mount (read-only)               | §2         |  [ ]   |
| 💎  |   5   | Write support (both platforms)                 | §3, §4     |  [ ]   |
| 💎  |   6   | USB auto-mount scripts                         | §3, §4     |  [ ]   |

---

## 1. Shared IXFS Core Parser
Port the IXFS on-disk structure parsing from kernel code to a standalone library. No kernel dependencies — uses `<stdint.h>`, `<string.h>`, `<stdio.h>`.

**Files:** `sdk/src/ixfs-mount/ixfs-core.c`, `ixfs-core.h`, `ixfs-structs.h`

- [x] Copy on-disk structures from `include/kernel/fs/ixfs.h` to `ixfs-structs.h`
- [x] `ixfs_open(disk_ctx)` — read and validate superblock
- [x] `ixfs_read_inode(ino)` — read inode from inode table
- [x] `ixfs_extent_lookup(inode, file_block)` — resolve file block to disk block
- [x] `ixfs_readdir(inode, callback)` — enumerate directory entries
- [x] `ixfs_lookup(parent_ino, name)` — find inode by name in directory
- [x] `ixfs_read_data(inode, offset, buf, len)` — read file data
- [ ] Commit: `"sdk: ixfs-mount shared IXFS parser — no kernel dependencies"`

**Test checkpoint (Linux + Windows):**
- Write a minimal `test_ixfs_core.c` that calls `ixfs_open()` on `build/system-disk.img` partition 2
- Output: `IXFS: "Impossible OS" v2, N blocks` — superblock parsed correctly
- Output: `ROOT: Impossible/ System/ ...` — root directory entries listed
- Read a known file and verify first bytes match expected content

## 2. Platform Disk I/O Layer
Abstracted raw sector read/write that works on both Windows and Linux.

**Files:** `sdk/src/ixfs-mount/ixfs-disk.c`, `ixfs-disk.h`

- [ ] `disk_open(path, partition_index)` — open device or image, locate partition via GPT
- [ ] `disk_read_sectors(lba, count, buf)` — read sectors within partition
- [ ] `disk_write_sectors(lba, count, buf)` — write sectors
- [ ] `disk_close()` — flush and close
- [ ] Windows: `CreateFile` + `ReadFile` / `WriteFile` with `SetFilePointerEx`
- [ ] Linux: `open` + `pread` / `pwrite`
- [ ] GPT parser: find partition by index, get start LBA and size
- [ ] `#ifdef _WIN32` / `#else` for platform split (or separate .c files)
- [ ] Commit: `"sdk: ixfs-mount platform disk I/O — Windows + Linux"`

**Test checkpoint (Linux + Windows):**
- `disk_open("build/system-disk.img", 2)` succeeds — GPT parsed, partition 2 found
- `disk_read_sectors(0, 1, buf)` returns 512 bytes with `0x49584653` at offset 0 (IXFS magic)
- Linux: `open()` + `pread()` path works; Windows: `CreateFile()` + `ReadFile()` path works

## 3. Linux FUSE Mount (Read-Only)
Implement libfuse3 callbacks for read-only mounting.

**Files:** `sdk/src/ixfs-mount/ixfs-fuse-linux.c`, `Makefile`

- [ ] `fuse_getattr` — stat from inode
- [ ] `fuse_readdir` — enumerate directory
- [ ] `fuse_open` — validate file access
- [ ] `fuse_read` — read file data via extent lookup
- [ ] Command: `ixfs-mount /dev/sdb2 /mnt/ixfs` or `ixfs-mount build/system-disk.img:2 /mnt/ixfs`
- [ ] `fusermount -u /mnt/ixfs` for unmount
- [ ] Makefile: `gcc -O2 ixfs-core.c ixfs-disk.c ixfs-fuse-linux.c -lfuse3 -o ../../tools/ixfs-mount`
- [ ] Commit: `"sdk: ixfs-mount Linux FUSE read-only mount"`

**Test checkpoint (Linux):**
- `./ixfs-mount build/system-disk.img:2 /mnt/ixfs` — mounts without error
- `ls /mnt/ixfs/` lists root directory (Impossible/, System/, etc.)
- `cat /mnt/ixfs/Impossible/boot.conf` reads file content correctly
- `stat /mnt/ixfs/Impossible/` shows directory attributes (mode, size, timestamps)
- `fusermount -u /mnt/ixfs` — clean unmount, no errors

## 4. Windows WinFsp Mount (Read-Only)
Implement WinFsp callbacks for read-only mounting as a drive letter.

**Files:** `sdk/src/ixfs-mount/ixfs-fuse-win.c`, `build-win.bat`

- [ ] WinFsp `GetVolumeInfo` — volume label, total/free space
- [ ] WinFsp `GetSecurityByName` — basic security descriptor
- [ ] WinFsp `Open` — lookup by path
- [ ] WinFsp `Read` — read file data
- [ ] WinFsp `ReadDirectory` — enumerate directory
- [ ] WinFsp `GetFileInfo` — size, timestamps, attributes
- [ ] WinFsp `Close` — release handle
- [ ] Command: `ixfs-mount.exe I: build\system-disk.img 2`
- [ ] build-win.bat: compile with MSVC or MinGW + WinFsp SDK
- [ ] Commit: `"sdk: ixfs-mount Windows WinFsp read-only mount"`

**Test checkpoint (Windows):**
- `ixfs-mount.exe I: build\system-disk.img 2` — mounts as drive I:
- `dir I:\` lists root directory in Explorer and cmd
- `type I:\Impossible\boot.conf` reads file content correctly
- Right-click drive → Eject or `ixfs-mount.exe --unmount I:` — clean unmount

## 5. Write Support (Both Platforms)
Add create, write, delete, rename operations.

**Files:** `sdk/src/ixfs-mount/ixfs-core.c`, `ixfs-fuse-linux.c`, `ixfs-fuse-win.c`

- [ ] `ixfs_write_data(inode, offset, buf, len)` — write file data, allocate blocks
- [ ] `ixfs_create(parent_ino, name, type)` — create file or directory
- [ ] `ixfs_delete(parent_ino, name)` — remove entry, free blocks
- [ ] `ixfs_rename(old_parent, old_name, new_parent, new_name)`
- [ ] Block allocator: port from `ixfs_alloc.c`
- [ ] Flush dirty inodes + superblock on unmount
- [ ] Wire into both FUSE backends
- [ ] Commit: `"sdk: ixfs-mount read-write support — both platforms"`

**Test checkpoint (Linux + Windows):**
- Mount R/W, create `I:\test-write.txt` (Windows) or `/mnt/ixfs/test-write.txt` (Linux) with known content
- Unmount cleanly
- Re-mount read-only, verify file exists and content matches
- Boot OS in QEMU, verify `C:\test-write.txt` exists with correct content
- Delete file from host, unmount, re-mount, verify file gone

## 6. USB Auto-Mount Scripts
Scripts to detect USB drives with IXFS partitions and mount them automatically.

**Files:** `sdk/scripts/mount-ixfs-usb.*`

### Windows (`mount-ixfs-usb.bat` + `mount-ixfs-usb.ps1`):
- [ ] Enumerate physical drives via `wmic diskdrive`
- [ ] For each removable drive: read GPT, find partition with IXFS type GUID
- [ ] Mount first IXFS partition as next available drive letter
- [ ] Log: `Mounted IXFS from USB (PhysicalDrive2, partition 3) as I:\`
- [ ] `unmount-ixfs.bat` — clean unmount

### Linux (`mount-ixfs-usb.sh`):
- [ ] Scan `/dev/sd*` and `/dev/nvme*` for removable/USB drives
- [ ] For each: read GPT via `sgdisk -p` or raw GPT parse, find IXFS partition
- [ ] Mount at `/mnt/ixfs` (create if needed)
- [ ] Log: `Mounted IXFS from /dev/sdb3 at /mnt/ixfs`
- [ ] Unmount: `fusermount -u /mnt/ixfs`

- [ ] Commit: `"sdk: USB auto-mount scripts for IXFS — Windows + Linux"`

**Test checkpoint (Linux + Windows):**
- Linux: plug USB boot drive, run `./mount-ixfs-usb.sh` — output: `Mounted IXFS from /dev/sdb2 at /mnt/ixfs`
- Windows: plug USB, run `mount-ixfs-usb.bat` — output: `Mounted IXFS from PhysicalDrive2, partition 2 as I:\`
- `ls /mnt/ixfs/` or `dir I:\` shows IXFS root directory
- Unmount script works cleanly on both platforms

---

## Build Instructions

**Linux:**
```bash
cd sdk/src/ixfs-mount
# Prerequisites: sudo apt install libfuse3-dev
make
# Output: sdk/tools/ixfs-mount
```

**Windows (MSVC):**
```cmd
cd sdk\src\ixfs-mount
REM Prerequisites: install WinFsp from https://winfsp.dev/
build-win.bat
REM Output: sdk\tools\ixfs-mount.exe
```

## OS Comparison

| ⭐ | Feature              | Win11                | Linux                | Impossible OS              |
|----|----------------------|----------------------|----------------------|----------------------------|
| 💎 | Cross-OS FS mount    | ✅ ext2fsd, Paragon  | ✅ ntfs-3g           | ⬜ §3+§4 IXFS both hosts  |
| ⭐ | USB auto-mount       | ❌ No custom FS      | ❌ No custom FS      | ⬜ §6 detect + mount USB  |
| ⭐ | Shared parser code   | ❌ Separate impls    | ❌ Separate impls    | ⬜ §1 single core, 2 back |
| ⭐ | Image:partition mount | ❌ Manual losetup    | ❌ Manual losetup    | ⬜ §2 `img:N` one-command |
| ⭐ | Snapshot browsing    | ❌ None              | ❌ None              | ⬜ Planned read-only snap |
| ⭐ | R/W from day one     | ⚠️ ext2fsd corrupts  | ⚠️ ntfs-3g slow      | ⬜ §5 journal-safe writes |

## Verification

- [ ] Linux: `ixfs-mount build/system-disk.img:2 /mnt/ixfs && ls /mnt/ixfs/`
- [ ] Windows: `ixfs-mount.exe I: build\system-disk.img 2` — browse in Explorer
- [ ] Write: create file from host, unmount, boot OS, verify file exists at `C:\`
- [ ] USB: plug USB drive, run mount script, IXFS mounted and browsable
