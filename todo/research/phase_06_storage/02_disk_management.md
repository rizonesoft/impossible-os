# 60 — Disk Management

## Overview

Partition manager, format disks, view drive usage. Like Windows Disk Management.

---

## Layout
```
┌──────────────────────────────────────────────────┐
│  Disk Management                            ─ □ ×│
├──────────────────────────────────────────────────┤
│  Drive     │ Size    │ Used    │ Free    │ FS    │
│  C:\ (OS)  │ 2.0 GB  │ 850 MB  │ 1.15 GB │ IXFS  │
│  D:\ (USB) │ 8.0 GB  │ 3.2 GB  │ 4.8 GB  │ FAT32 │
├──────────────────────────────────────────────────┤
│  Disk 0: 2.0 GB                                  │
│  ┌────────────────────────────┬─────────────┐    │
│  │   C:  (IXFS, 2.0 GB)      │ Unallocated │    │
│  └────────────────────────────┴─────────────┘    │
│  Disk 1: 8.0 GB (USB)                           │
│  ┌──────────────────────────────────────────┐    │
│  │   D:  (FAT32, 8.0 GB)                    │    │
│  └──────────────────────────────────────────┘    │
└──────────────────────────────────────────────────┘
```

## Features
- View partition layout (graphical bar)
- Create / delete partitions
- Format partition (FAT32, IXFS)
- Assign drive letters
- View disk SMART status (future)

## Depends on: `23_disk_persistent_fs.md` (disk drivers + partition tables)
## Files: `src/apps/diskmgr/diskmgr.c` (~500 lines)
## Implementation: 1-2 weeks (after disk drivers exist)
