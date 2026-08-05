---
schema_version: 1
id: disk-inspect
domain: 14-host-tools
status: active
title: "TODO-06 -- disk-inspect (Disk Image Browser)"
---

# TODO-06 -- disk-inspect (Disk Image Browser)

> **Goal:** Interactively browse GPT partition tables, IXFS superblocks, inode tables, extent maps, and raw sectors from a disk image or physical drive. The `fdisk -l` + `debugfs` equivalent for Impossible OS formats.

## Outcome

```
$ disk-inspect build/system-disk.img
  GPT Disk: 512 MiB, 1048576 sectors
  Partition 1: EFI System    64 MiB  FAT32   LBA 2048-133119
  Partition 2: Basic Data    16 MiB  FAT32   LBA 133120-165887
  Partition 3: IXFS          430 MiB IXFS    LBA 165888-1046527

> inspect 3
  IXFS v2 "Impossible OS"
  Block size: 4096, Total: 110331, Free: 105382
  Inodes: 128, Used: 55

> ls /
  .                   (dir,  inode 1)
  Impossible/         (dir,  inode 2)
  cmd.exe             (file, inode 53, 44968 bytes)
  hello.txt           (file, inode 54, 25 bytes, inline)

> inode 53
  Type: regular file
  Size: 44968 bytes
  Extents: [blk 100, len 11]
  Created: 2026-03-29 17:37:34

> hexdump 53 0 64
  00000000: 7f 45 4c 46 02 01 01 00  00 00 00 00 00 00 00 00  .ELF............
```

## Implementation Order

| ⭐  | Order | Deliverable                       | Depends On | Status |
| --- | :---: | --------------------------------- | ---------- | :----: |
| 💎  |   1   | GPT partition table parser        | --         |  [ ]   |
| 💎  |   2   | IXFS superblock + inode inspector | §1         |  [ ]   |
| 💎  |   3   | Directory listing (ls, tree)      | §2         |  [ ]   |
| 💎  |   4   | Hex dump and raw sector read      | §1         |  [ ]   |
| 💎  |   5   | Interactive shell (REPL)          | §1-§4      |  [ ]   |

---

## 1. GPT Partition Table Parser
Read and display GPT header, partition entries, type GUIDs.

- [ ] Parse GPT header (LBA 1): signature, partition count, entry size
- [ ] Parse partition entries: name, type GUID, start/end LBA, attributes
- [ ] Identify known types: EFI System, Basic Data, IXFS (by GUID)
- [ ] Display: formatted table with sizes and filesystem types

## 2. IXFS Superblock + Inode Inspector
Parse IXFS metadata and display detailed inode information.

- [ ] Read superblock: magic, version, block size, counts
- [ ] Read inode table: iterate all inodes, show used/free
- [ ] `inode N`: display type, size, extent list, timestamps, permissions
- [ ] Validate: check for inconsistencies (size vs extent coverage)

## 3. Directory Listing
Browse the IXFS directory tree.

- [ ] `ls /path` -- list directory entries with type, inode, size
- [ ] `tree /path` -- recursive directory tree
- [ ] `cat /path/file` -- display file contents (text mode)
- [ ] `stat /path` -- detailed inode info for a path

## 4. Hex Dump and Raw Sector Read
Low-level inspection of raw disk data.

- [ ] `hexdump inode offset length` -- hex dump of file data
- [ ] `sector LBA count` -- raw sector hex dump
- [ ] `block N` -- IXFS block hex dump
- [ ] `bitmap` -- display block allocation bitmap (visual)

## 5. Interactive Shell
REPL for browsing without restarting the tool.

- [ ] Command loop with prompt: `disk-inspect>`
- [ ] Tab completion for paths and commands
- [ ] History (readline on Linux, basic on Windows)
- [ ] `help` command listing all available commands

## Verification

- [ ] Parse `build/system-disk.img` correctly on both platforms
- [ ] `ls /` shows expected files
- [ ] `hexdump` of cmd.exe starts with ELF magic
- [ ] Interactive shell works with command history
