---
schema_version: 1
id: ixfs-fsck
domain: 14-host-tools
status: active
title: "TODO-07 -- ixfs-fsck (Filesystem Consistency Checker)"
---

# TODO-07 -- ixfs-fsck (Filesystem Consistency Checker)

> **Goal:** Verify IXFS filesystem integrity: superblock validity, inode consistency, extent coverage, block bitmap accuracy, orphan detection, and optional repair. Run after a crash or before any disk image release.

## Outcome

```
$ ixfs-fsck build/system-disk.img 3
  IXFS v2 "Impossible OS" -- checking partition 3...

  [1/6] Superblock.............. OK (v2, 110331 blocks, 128 inodes)
  [2/6] Block bitmap............ OK (105382 free, 4949 used)
  [3/6] Inode table............. OK (55 used, 73 free)
  [4/6] Extent integrity........ OK (all extents within bounds)
  [5/6] Directory structure..... OK (no orphan inodes)
  [6/6] Cross-reference......... OK (bitmap matches extent usage)

  CLEAN -- no errors found (0.3s)
```

```
$ ixfs-fsck --repair build/system-disk.img 3
  [4/6] Extent integrity........ ERROR: inode 42 extent [blk 999999, len 5] out of bounds
        REPAIR: truncated inode 42 to last valid extent
  [5/6] Directory structure..... WARN: inode 37 not referenced by any directory
        REPAIR: moved to /lost+found/inode_37

  REPAIRED -- 1 error fixed, 1 orphan recovered
```

## Implementation Order

| ⭐  | Order | Deliverable                            | Depends On | Status |
| --- | :---: | -------------------------------------- | ---------- | :----: |
| 💎  |   1   | Superblock validation                  | --         |  [ ]   |
| 💎  |   2   | Block bitmap verification              | §1         |  [ ]   |
| 💎  |   3   | Inode table scan                       | §1         |  [ ]   |
| 💎  |   4   | Extent integrity check                 | §3         |  [ ]   |
| 💎  |   5   | Directory structure + orphan detection | §3, §4     |  [ ]   |
| 💎  |   6   | Cross-reference (bitmap vs extents)    | §2, §4     |  [ ]   |
| ⭐  |   7   | Repair mode (--repair)                 | §1-§6      |  [ ]   |

---

## 1. Superblock Validation
Verify superblock magic, version, sizes, and field consistency.

- [ ] Reconcile with the shipped kernel checker before writing any pass: build the host tool from `ixfs_fsck()` logic, not a second implementation
  - `src/kernel/fs/ixfs/ixfs_fsck.c` (owner `01-boot-platform/TODO-22` §3) already checks superblock CRC32C, data checksums, refcounts, snapshots and the journal, and repairs only a safe subset; this plan predates it and omits those checks.
- [ ] Magic number matches IXFS signature
- [ ] Version is supported (v2)
- [ ] Block size is power of 2 (4096)
- [ ] Total blocks and inode count are reasonable for partition size
- [ ] Free block count ≤ total blocks

## 2. Block Bitmap Verification
Scan the block allocation bitmap for consistency.

- [ ] Count set bits -- must match `total - free` from superblock
- [ ] No bits set beyond total block count
- [ ] Superblock, inode table, bitmap blocks marked as used

## 3. Inode Table Scan
Walk all inodes and verify field integrity.

- [ ] Each used inode has valid type (file, directory, symlink)
- [ ] Size is consistent with extent coverage
- [ ] Timestamps are reasonable (not 0, not future)
- [ ] Link count ≥ 1 for used inodes

## 4. Extent Integrity Check
Verify all extent entries point to valid blocks.

- [ ] Each extent: start block within [0, total_blocks)
- [ ] Each extent: start + length within [0, total_blocks]
- [ ] No overlapping extents between different inodes
- [ ] Total extent coverage matches inode size (rounded up to block size)

## 5. Directory Structure + Orphan Detection
Walk the directory tree from root and find unreachable inodes.

- [ ] BFS/DFS from root inode -- mark all reachable inodes
- [ ] Inodes marked used but not reachable = orphans
- [ ] Directory entries point to valid inodes
- [ ] No directory cycles (parent loops)
- [ ] `.` and `..` entries correct in every directory

## 6. Cross-Reference
Verify that the block bitmap exactly matches the blocks referenced by extents.

- [ ] Build "expected bitmap" from all inode extents + metadata blocks
- [ ] Compare with on-disk bitmap
- [ ] Report: leaked blocks (in bitmap but no extent), phantom blocks (in extent but not bitmap)

## 7. Repair Mode
Fix detected issues when `--repair` flag is given.

- [ ] Out-of-bounds extents: truncate inode to last valid extent
- [ ] Orphan inodes: create `/lost+found/` directory, link orphans there
- [ ] Bitmap mismatch: rebuild bitmap from extents (authoritative)
- [ ] Bad superblock free count: recalculate from bitmap
- [ ] Always ask before destructive repairs (unless `--yes`)

## Verification

- [ ] Clean image: `ixfs-fsck build/system-disk.img 3` reports CLEAN
- [ ] Corrupt image: introduce known corruption, verify detection
- [ ] Repair: fix corruption, verify image boots correctly after
- [ ] Works on both Linux and Windows
