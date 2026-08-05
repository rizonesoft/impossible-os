---
schema_version: 1
id: ntfs-readwrite
domain: 05-storage-filesystems
status: active
title: "TODO-02 -- NTFS Read/Write Driver"
---

# TODO-02 -- NTFS Read/Write Driver

> **Goal:** Complete the three remaining NTFS write pillars -- B+ tree directory mutation, full data write/truncate path, and volume formatter -- plus wire dirty-volume recovery into mount, implement `$Secure` security descriptors, and add an end-to-end format/crash/replay test. This delivers full read/write access to any Windows-formatted external drive.

> [!IMPORTANT]
> **Purpose:** NTFS is used to access **external Windows drives** -- USB drives, secondary HDD/SSD formatted by Windows, dual-boot Windows partitions. The Impossible OS system drive `C:\` remains IXFS permanently. §1–13 (BPB, MFT, attributes, data runs, B+ tree read, VFS registration, LZNT1, MFT cache, write foundations, file CRUD, `$LogFile` journal) are **done**. This TODO delivers only the remaining items. Verify `ntfs_vfs.c` wires §1, §2, and §5 (dirty recovery) into the mount/unmount paths before marking any section complete.

## Inputs

- `src/kernel/fs/ntfs/ntfs_index_insert.c` -- `ntfs_index_insert()` core exists; §1 audits split cascade completeness and journals all ops
- `src/kernel/fs/ntfs/ntfs_index_delete.c` -- `ntfs_index_delete()` core exists; §1 audits rebalance/merge completeness and journals all ops
- `src/kernel/fs/ntfs/ntfs_data_write.c` -- `ntfs_write_data()` and `ntfs_truncate()` exist; §2 wires them into `ntfs_vfs.c` and audits journaling coverage
- `src/kernel/fs/ntfs/ntfs_recovery.c` -- `ntfs_recovery_replay()` exists; §5 wires it into `ntfs_vfs.c` mount path on `vol->volume_dirty`
- `src/kernel/fs/ntfs/ntfs_metadata.c` -- `$Secure` lookup stubbed at line ~312; §4 fills it in
- `src/kernel/fs/ntfs/ntfs_vfs.c` -- mount/unmount path; §2 + §5 extend it; §3 adds `ntfs_format()`
- `src/kernel/fs/ntfs/ntfs_test.c` -- existing test suite; §6 extends it with the format+crash+replay scenario
- → XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §7` -- the LRU sector cache must be invalidated (`cache_invalidate(dev)`) at NTFS unmount; verify the unmount path calls this
- → XREF: `10-platform-services` -- `$Secure` security descriptors (§4) provide the on-disk format that maps to the kernel security token model; coordinate with the security TODO for how `Everyone:Full-Control` maps to kernel ACL structs

## Outcome

- B+ tree insert/delete fully journaled; page splits and merges complete with `$INDEX_ALLOCATION` `$BITMAP` updates.
- `ntfs_write_data()` and `ntfs_truncate()` wired into `ntfs_vfs.c`; all data modifications journaled.
- `ntfs_format(blkdev, label, cluster_size)` creates a mountable NTFS volume with all 16 system files.
- `$Secure` `$SDS`/`$SDH`/`$SII` streams written; newly created files stamped with a default security descriptor.
- Dirty-volume auto-recovery fires on mount when `VOLUME_IS_DIRTY` is set; volume always mounts clean.
- End-to-end test: 64 MiB in-memory image, 100 files, crash simulation, journal replay, fsck passes.

## Implementation Order

| ⭐  | Order | Deliverable                                                                          | Depends On                                           | Status |
| --- | :---: | ------------------------------------------------------------------------------------ | ---------------------------------------------------- | :----: |
| 💎  |   1   | §1 B+ tree mutation audit + journaling -- split/merge completeness, `$BITMAP` updates | `ntfs_index_insert/delete.c` cores, `$LogFile` engine |  [ ]   |
| 💎  |   2   | §2 Write data path wiring -- `ntfs_write_data/truncate` → `ntfs_vfs.c`, journaled    | §1 (index updated on cluster alloc), `ntfs_data_write.c` |  [ ]   |
| 💎  |   3   | §3 NTFS volume initialiser -- `ntfs_format()`, VBR, 16 system files, `$Bitmap`       | §2 (write path proven reliable)                      |  [ ]   |
| 💎  |   4   | §4 `$Secure` security stream -- `$SDS`/`$SDH`/`$SII`, default descriptor on create   | §3 (system file creation baseline), §2 (write path)  |  [ ]   |
| 💎  |   5   | §5 Dirty volume recovery wiring -- `vol->volume_dirty` → `ntfs_recovery_replay()` at mount | `ntfs_recovery_replay()` exists (ntfs_recovery.c) |  [ ]   |
| 💎  |   6   | §6 Format + crash + replay test -- 64 MiB image, 100 files, dirty crash, fsck verify  | §1–5 all complete                                    |  [ ]   |

> All six sections are `💎` parity: they bring NTFS to the same read/write level as `ntfs3.ko` on Linux and `NTFS.sys` on Windows. There is no `⭐` exclusive claim here -- the goal is correctness and reliability, not architectural differentiation.

---

## 1. B+ Tree Mutation Audit + Journaling `[Opus]`

Audit `ntfs_index_insert()` and `ntfs_index_delete()` for split/merge completeness. Ensure all tree mutations are logged via the existing `$LogFile` journal engine and that `$BITMAP` is updated on every INDX buffer alloc/free.

**Files:** `src/kernel/fs/ntfs/ntfs_index_insert.c` (extend), `src/kernel/fs/ntfs/ntfs_index_delete.c` (extend), `src/kernel/fs/ntfs/ntfs_index_helpers.c` (extend)

> [!NOTE]
> Split cascade: when a leaf page overflows, the median entry propagates to the parent; if the parent also overflows, split propagates upward -- potentially to the root `$INDEX_ROOT` attribute. If root overflows, a new `$INDEX_ALLOCATION` INDX buffer is allocated, root demoted. `$BITMAP` bit for each INDX VCN must be set on alloc and cleared on free. All index writes (INDX buffer, `$INDEX_ROOT` attribute, `$BITMAP` bit flip) must be bracketed by `ntfs_journal_log_begin()` / `ntfs_journal_log_commit()` so the journal can replay them on dirty recovery.

- [ ] Audit `ntfs_index_insert()`: trace the split cascade path -- verify it handles root overflow by demoting root into a new INDX buffer and allocating `$INDEX_ALLOCATION` VCN; verify `$BITMAP` bit is set for each newly allocated INDX VCN
- [ ] Audit `ntfs_index_delete()`: verify borrow-from-sibling and merge-with-sibling both handle the parent separator removal correctly; verify `$BITMAP` bit is cleared when an empty INDX page is freed; verify merge cascades to root without overflow
- [ ] Journal wrappers: every INDX buffer write and `$INDEX_ROOT` attribute modification must call `ntfs_journal_log_record(vol, redo_op, undo_op, data)` using the existing journal API; verify no index mutation bypasses the journal
- [ ] `ntfs_index_alloc_indx(vol, dir_inode, &vcn)`: allocate next free VCN in `$INDEX_ALLOCATION`; set `$BITMAP` bit; journal the `$BITMAP` change; return VCN
- [ ] `ntfs_index_free_indx(vol, dir_inode, vcn)`: clear `$BITMAP` bit; journal the change; mark INDX buffer stale in cache
- [ ] Integration test: create directory; insert 500 filenames (forces multiple splits); delete 250 filenames (forces merges); verify B+ tree invariant (`all keys sorted, all leaves at same depth`) via `ntfs_index_check(vol, dir_inode)`
- [ ] Commit: `"fs/ntfs: B+ tree mutation -- split cascade, merge, $BITMAP, full journal coverage"`

## 2. Write Data Path Wiring `[Sonnet]`

Wire `ntfs_write_data()` and `ntfs_truncate()` from `ntfs_data_write.c` into `ntfs_vfs.c`'s VFS operation table. Confirm all cluster allocation and `$DATA` attribute extension are journaled.

**Files:** `src/kernel/fs/ntfs/ntfs_vfs.c` (extend), `src/kernel/fs/ntfs/ntfs_data_write.c` (audit)

> [!NOTE]
> `ntfs_write_data(vol, inode, offset, buf, len)` handles within-cluster read-modify-write, cluster allocation for extent append, and `$DATA` run-list extension. `ntfs_truncate(vol, inode, new_size)` frees tail clusters and shrinks the run list. Both must update `$STANDARD_INFORMATION` timestamps (modification + MFT changed). The VFS `write` operation must propagate the return value of `ntfs_write_data()` to the VFS caller as bytes-written or `-EIO`.

- [ ] Wire `vfs_ops.write = ntfs_vfs_write` in `ntfs_vfs.c`; implement `ntfs_vfs_write(vfs_node, buf, offset, len)` → call `ntfs_write_data(vol, inode, offset, buf, len)`; on error return `-EIO`; on success return bytes written
- [ ] Wire `vfs_ops.truncate = ntfs_vfs_truncate` → call `ntfs_truncate(vol, inode, new_size)`
- [ ] Audit `ntfs_write_data()`: verify `ntfs_alloc_clusters()` call journals the `$Bitmap` cluster allocation; verify `$DATA` attribute length update in MFT is journaled
- [ ] Audit `ntfs_truncate()`: verify freed cluster range is journaled in `$LogFile` before `$Bitmap` bits are cleared; verify run-list shrink is journaled
- [ ] Dirty-on-write: before the first write to any mounted volume, set `vol->volume_dirty = 1` and write `VOLUME_IS_DIRTY` flag to `$Volume` `FLAGS` field; cleared by clean unmount or journal replay
- [ ] Clean unmount: `ntfs_vfs_unmount()` → call `cache_flush(dev)` (block cache) → `ntfs_journal_checkpoint(vol)` → clear `VOLUME_IS_DIRTY` in `$Volume`
- [ ] Compressed-write `$STANDARD_INFORMATION` timestamps: `ntfs_write_compressed_data()` skips `update_std_info_times()`, leaving compressed writes with stale mod/MFT-change times; stamp before the compressed MFT commit. (TODO-08 §13 review.)
- [ ] Commit: `"fs/ntfs: wire write/truncate into VFS, dirty-on-write flag, clean unmount"`

## 3. NTFS Volume Initialiser `[Opus]`

Implement `ntfs_format(blkdev, label, cluster_size)` to create a mountable NTFS volume with VBR, `$MFT`, `$MFTMirr`, all 16 system files, and `$Bitmap`.

**Files:** `src/kernel/fs/ntfs/ntfs_format.c` (new), `include/kernel/fs/ntfs.h` (extend)

> [!NOTE]
> NTFS volume layout: sector 0 = VBR (BPB: `bytes_per_sector=512`, `sectors_per_cluster`, `mft_start_lcn`, `mft_mirror_start_lcn`, `clusters_per_mft_record=-10` for 1 KiB records, `clusters_per_index_buffer=-3` for 4 KiB). MFT inode table: inode 0=`$MFT`, 1=`$MFTMirr`, 2=`$LogFile`, 3=`$Volume`, 4=`$AttrDef`, 5=`.` (root dir), 6=`$Bitmap`, 7=`$Boot`, 8=`$BadClus`, 9=`$Secure`, 10=`$UpCase`, 11=`$Extend`, 12–15=reserved. `$Bitmap` covers the entire volume; bits for VBR, MFT zone, and system files must be pre-set.

- [ ] `ntfs_format(blkdev_t *dev, const char *label, uint32_t cluster_size)`: compute geometry (sector count, cluster count, MFT zone), write VBR (sector 0), write backup VBR (last sector)
- [ ] Build MFT: allocate a contiguous MFT zone (12.5% of volume); write MFT inode records for inodes 0–15; each record has `FILE` magic, `$STANDARD_INFORMATION` + primary attribute(s)
- [ ] System file attributes:
  - `$MFT` (0): `$DATA` run list pointing to MFT zone
  - `$LogFile` (2): `$DATA` run list; initialise log file header with restart area
  - `$Volume` (3): `$VOLUME_NAME` (label), `$VOLUME_INFORMATION` (NTFS version 3.1, flags=0)
  - `$Bitmap` (6): `$DATA` run list; write `$Bitmap` with MFT zone + VBR + system clusters pre-allocated
  - `$Boot` (7): `$DATA` resident; copy VBR bytes
  - Root dir (5): `$INDEX_ROOT` (`$I30`, B+ tree) with `.` and `..` self-referential entries
  - `$Secure` (9): empty `$SDS`/`$SDH`/`$SII` streams (populated by §4 after format)
  - `$UpCase` (10): `$DATA` run list; write 128 KiB Unicode uppercase table
- [ ] Write `$MFTMirr` (inode 1): first 4 MFT records mirrored at volume midpoint
- [ ] Register in shell `format` command: `format D: /fs:ntfs /q` → `ntfs_format(dev, label, 4096)`
- [ ] Boot log: `[NTFS] Formatted volume "%s" -- %llu clusters, %u B/cluster, MFT @ LCN %llu`
- [ ] Commit: `"fs/ntfs: ntfs_format() -- VBR, 16 system files, $Bitmap, $UpCase, $MFTMirr"`

## 4. `$Secure` Security Stream `[Sonnet]`

Implement `$Secure` stream write support: initialise `$SDS`/`$SDH`/`$SII` B+ tree indexes during format, and stamp a default security descriptor (`Everyone:Full-Control`) on each newly created file.

**Files:** `src/kernel/fs/ntfs/ntfs_secure.c` (new), `include/kernel/fs/ntfs_internal.h` (extend), `src/kernel/fs/ntfs/ntfs_metadata.c` (fill stub at line ~312)

> [!NOTE]
> `$Secure` inode 9 has three streams: `$SDS` (Security Descriptor Stream -- flat array of `SECURITY_DESCRIPTOR_HEADER` + raw SD blobs), `$SDH` (B+ tree index by SD hash for dedup), `$SII` (B+ tree index by `security_id` for fast lookup by inode). A newly created file's MFT record gets a `security_id` field in `$STANDARD_INFORMATION` pointing into `$SII`. Default SD: owner=BUILTIN\Administrators SID `S-1-5-32-544`, DACL with one ACE: `ACCESS_ALLOWED_ACE(Everyone S-1-1-0, mask=GENERIC_ALL)`.

- [ ] `ntfs_secure_init(vol)`: on mount, verify `$Secure` inode 9 `$SDS`/`$SDH`/`$SII` streams exist; cache `$SDS` size and next `security_id` in `vol->secure_next_id`
- [ ] `ntfs_secure_write_sd(vol, sd_buf, sd_len, &security_id)`: hash SD (`crc32c(sd_buf, sd_len)`); look up `$SDH` index -- if entry exists, return existing `security_id` (dedup); else append to `$SDS`, insert into `$SDH` and `$SII`, increment `vol->secure_next_id`; journal all index mutations
- [ ] Default SD: `ntfs_secure_default_sd(uint8_t *buf, uint32_t *len)`: build binary SD with owner SID `S-1-5-32-544`, DACL containing single `ACCESS_ALLOWED_ACE` for Everyone (`S-1-1-0`, `mask=0x1F01FF`)
- [ ] Fill `ntfs_metadata.c` stub (~line 312): `ntfs_get_security_descriptor(vol, inode)` → read `security_id` from `$STANDARD_INFORMATION`; look up in `$SII` index; read `$SDS` entry; return SD pointer
- [ ] `ntfs_file_create()` / `ntfs_mkdir()`: after MFT record write, call `ntfs_secure_write_sd(vol, default_sd, default_sd_len, &sid)`, store `sid` in `$STANDARD_INFORMATION.security_id`
- [ ] Boot log: `[NTFS] $Secure loaded -- %u descriptors, next_id=%u`
- [ ] Commit: `"fs/ntfs: $Secure -- $SDS/$SDH/$SII streams, default SD on file create, security_id lookup"`

## 5. Dirty Volume Recovery Wiring `[Sonnet]`

Wire `ntfs_recovery_replay()` (which already exists in `ntfs_recovery.c`) into the `ntfs_vfs.c` mount path so a dirty volume is automatically recovered before returning a VFS handle.

**Files:** `src/kernel/fs/ntfs/ntfs_vfs.c` (extend)

> [!NOTE]
> The mount sequence must be: read BPB → init MFT cache → read `$Volume` FLAGS → if `VOLUME_IS_DIRTY (bit 0)` set in `$VOLUME_INFORMATION`, call `ntfs_recovery_replay(vol)` before registering the VFS node → if replay returns error: log warning, mount read-only, set `vol->read_only = 1`. This matches Windows' chkdsk-on-dirty and Linux's `ntfs3` `ntfs_load_sb()` dirty check.

- [ ] In `ntfs_vfs_mount()`: after MFT cache init, read `$Volume` inode 3 `$VOLUME_INFORMATION` attribute; check `flags & NTFS_VOLUME_IS_DIRTY (0x0001)`; if set: log `[NTFS] Dirty volume on %s -- running journal replay`; call `ntfs_recovery_replay(vol)`
- [ ] On replay success: `vol->volume_dirty = 0`; log `[NTFS] Recovery complete -- %u transactions replayed`; continue mount normally
- [ ] On replay failure: log `[NTFS] Recovery failed on %s -- mounting read-only`; set `vol->read_only = 1`; return VFS handle with write ops nulled out
- [ ] Verify `ntfs_vfs_unmount()` calls `ntfs_journal_checkpoint(vol)` and clears `VOLUME_IS_DIRTY` in `$Volume` before returning (complement of §2 dirty-on-write)
- [ ] Commit: `"fs/ntfs: wire dirty volume recovery -- mount checks VOLUME_IS_DIRTY, ntfs_recovery_replay()"`

## 6. NTFS Format + Crash + Replay Test `[Sonnet]`

Extend `ntfs_test.c` with an end-to-end scenario: format a 64 MiB in-memory image, write/read/rename/delete 100 files across all size classes, simulate a crash, verify journal replay, and confirm no corruption via an fsck-equivalent walk.

**Files:** `src/kernel/fs/ntfs/ntfs_test.c` (extend)

> [!NOTE]
> In-memory block device: allocate 64 MiB with `pmm_alloc_contiguous()` (or a test-mode ramdisk); wrap in a `blkdev_t` with read/write callbacks that memcpy from the buffer. File size classes: inline (≤ 700 bytes resident), single-run (1–64 KiB), multi-run (> 64 KiB spanning multiple cluster allocations). Crash simulation: after writing 50 files, directly set `vol->volume_dirty = 1` and write `VOLUME_IS_DIRTY` to `$Volume` without calling `ntfs_journal_checkpoint()`, then unmount without clean close. Re-mount triggers dirty recovery.

- [ ] `ntfs_test_format_and_rw()`: call `ntfs_format(ramdisk, "TESTDRIVE", 4096)`; mount via `ntfs_vfs_mount()`
- [ ] Write 100 files with varying content sizes: 20 inline (100–700 bytes), 40 single-run (4 KiB–64 KiB), 40 multi-run (128 KiB–512 KiB); verify each file reads back identical content
- [ ] Rename 30 files: `ntfs_rename(vol, old_path, new_path)`; verify directory index reflects new names; old names absent
- [ ] Delete 20 files: verify MFT records freed, `$Bitmap` cluster bits cleared, directory index entries removed
- [ ] Crash simulation: set dirty flag without clean unmount; re-mount; verify `ntfs_recovery_replay()` is called; verify all 80 surviving files read back correctly; no phantom entries in directory
- [ ] fsck walk: `ntfs_fsck(vol)` -- traverse every MFT record: verify `FILE` magic, attribute chain integrity, run list cluster ranges within volume bounds, directory index sorted order; return 0 on clean, negative on corruption
- [ ] Log: `[NTFS-TEST] format+rw+crash+replay: PASS` or `FAIL: <reason>`
- [ ] Commit: `"fs/ntfs: test -- format+rw+crash+replay, 100 files, fsck walk, dirty recovery verify"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | NTFS B+ tree insert/delete with split/merge + `$LogFile` journaling | ✅ `NTFS.sys`; full B+ tree mutation;    | ✅ `ntfs3.ko`; B+ tree insert/delete; journal; | ⚠️ §1 -- Partial -- cores exist; audits   |
| 💎  | NTFS data write                          | ✅ `NTFS.sys`; full write path; `NtWriteFile`/`NtSetEndOfFile` | ✅ `ntfs3.ko`; `file_write_iter()`; cluster alloc + | ⚠️ §2 -- Partial -- `ntfs_write_data()`/`ntfs_truncate()` exist; wires |
| 💎  | NTFS volume format                       | ✅ `format.exe`; in-kernel `NTFS.sys` init | ✅ `mkntfs` (`ntfsprogs`); full volume initialiser | ⬜ §3 -- `ntfs_format()`, VBR, MFT system files |
| 💎  | `$Secure` security stream                | ✅ `NTFS.sys`; `$Secure` full; `GetFileSecurity`/`SetFileSecurity` | ✅ `ntfs3.ko` reads `$Secure`; write support | ⬜ §4 -- `ntfs_secure.c`, SD write with CRC32c |
| 💎  | Dirty volume auto-recovery               | ✅ `NTFS.sys`; chkdsk-on-dirty or replay at | ✅ `ntfs3.ko`; dirty flag check in       | ⚠️ §5 -- Partial -- `ntfs_recovery_replay()` exists; wires |
| 💎  | End-to-end format + crash + replay regression test | ✅ Internal Windows test suites (not     | ✅ `ntfsck` (`ntfsprogs`); `ntfs3` upstream LTP | ⬜ §6 -- in-kernel `ntfs_test_format_and_rw()` + `ntfs_fsck()` automated |

> **After §1–6:** Impossible OS NTFS reaches full read/write parity with `ntfs3.ko` on Linux. External Windows drives -- USB sticks, SSDs, dual-boot partitions -- mount cleanly, survive dirty disconnects via journal replay, and receive correctly stamped security descriptors on new files. This is a correctness goal; the differentiator is that the entire stack runs in-kernel with no FUSE indirection, matching Windows' own `NTFS.sys` architecture rather than Linux's user-space `ntfs-3g` fallback.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] B+ tree: create directory, insert 500 files → serial shows no split errors; delete 250 → `ntfs_index_check()` passes; `$BITMAP` bits match allocated INDX pages
- [ ] Write path: write 1 MiB file to mounted NTFS image → serial shows `[NTFS] $DATA extended, 256 clusters allocated`; read back: byte-for-byte identical; `fsck` on image passes
- [ ] Truncate: create 512 KiB file, truncate to 10 KiB → `$Bitmap` shows 127 clusters freed; read confirms 10 KiB content correct
- [ ] Format: `ntfs_format(ramdisk, "TEST", 4096)` → mount succeeds; root dir contains `.`/`..`; `$Volume` name = `TEST`; `$Bitmap` consistent
- [ ] `$Secure`: create file after format → `$STANDARD_INFORMATION.security_id != 0`; `ntfs_get_security_descriptor()` returns Everyone:Full-Control descriptor; `$SII` index lookup succeeds
- [ ] Dirty recovery: simulate crash (dirty flag, no clean unmount) → re-mount shows `[NTFS] Dirty volume -- running journal replay` → `[NTFS] Recovery complete`; all surviving files intact
- [ ] Read-only fallback: corrupt `$LogFile` → mount logs `[NTFS] Recovery failed -- mounting read-only`; write returns `-EROFS`
- [ ] End-to-end test: `ntfs_test_format_and_rw()` runs and logs `[NTFS-TEST] format+rw+crash+replay: PASS`
- [ ] Commit: `"fs/ntfs: complete write pillars -- B+ tree, write path, format, $Secure, dirty recovery, tests"`
