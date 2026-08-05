---
schema_version: 1
id: partition-tools-storage-suite
domain: 05-storage-filesystems
status: active
title: "TODO-13 -- Partition Management & Storage Tools"
---

# TODO-13 -- Partition Management & Storage Tools

> **Goal:** Deliver the complete partition management and disk tool suite: GPT + MBR partition write APIs, the `diskpart` interactive CLI, `chkdsk` / `defrag` / `sfc` / `recover` / `diskuse` / `snapshot` CLI commands, and a Disk Management GUI. Block devices work and filesystems mount -- this TODO turns raw disk infrastructure into a fully operational partition and storage administration layer.

> [!IMPORTANT]
> GPT read infrastructure is complete: `gpt_parse()`, `guid_generate()` (RDRAND-backed), `gpt_sync_backup()`, `gpt_crc32()`, and `gpt_guid_equal()` all exist in `src/kernel/fs/gpt.c`. MBR parse + EBR chain is also done. `src/shell/` is currently empty (`.gitkeep` only) -- all shell commands are new. Sections §1–§2 are **kernel-layer** write APIs; §3–§10 are **shell + GUI** tools that depend on them. §6 (`sfc`) requires a build-time manifest generation step (`scripts/`); §7 (`recover`) depends on the IXFS snapshot API (→ XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §8`). Destructive operations (`delete`, `format`, `wipe`) must require explicit user confirmation in both CLI and GUI.

## Inputs

- `src/kernel/fs/gpt.c` + `include/kernel/fs/gpt.h` -- `gpt_parse()`, `guid_generate()`, `gpt_sync_backup()`, `gpt_crc32()`, `gpt_guid_equal()`, `gpt_type_name()` all exist; §1 adds write-path functions on top
- `src/kernel/fs/mbr.c` + `include/kernel/fs/mbr.h` -- MBR parse, EBR chain, `mbr_type_is_extended()` done; §2 adds create/delete/CHS-encode/write functions
- `src/kernel/drivers/blkdev.c` + `include/kernel/drivers/blkdev.h` -- `blkdev_write()`, `blkdev_read()`, `blkdev_discard()` for TRIM in §5
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §5` -- `vfs_auto_assign_letters()` called after partition CRUD in §1–§9 to update drive-letter mappings; `mount`/`umount` commands owned there
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §1` -- IRP engine must be complete before any GUI tool issues file I/O
- → XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §8` -- `ixfs_snapshot_create/list/restore/delete()` are the backend for §10
- → XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3` -- IXFS defrag backend (`FSCTL_DEFRAGMENT_FILE`) used by §5
- Related (no stable XREF target): `09-desktop-shell/TODO-xx-window-manager` -- GUI panels for §4, §5, §6, §7, §8, §9, §10 require compositor/widget layer

## Outcome

- `gpt_partition_create/delete/resize`: GPT write path with dual-header CRC update and atomic sector writes.
- `mbr_partition_create/delete`: MBR write path with CHS encoding and slot management.
- `diskpart` interactive CLI: full CRUD -- list, create, delete, format, assign, active, info.
- `chkdsk` CLI + GUI: per-filesystem validation and repair with real-time progress; schedule-on-next-boot for system drive.
- `defrag` CLI + GUI: fragmentation analysis, journal-safe block relocation, SCHED_IDLE background thread, TRIM.
- `sfc` CLI + GUI: build-time manifest, runtime CRC compare, repair from recovery image.
- `recover` CLI + GUI: deleted file recovery (IXFS, FAT32, data carving) with Recovery Wizard.
- Disk Management GUI: two-panel split view with proportional partition bar and SMART status.
- `diskuse` CLI + GUI: recursive directory size walker with treemap visualization.
- Snapshot manager CLI + GUI: IXFS snapshot CRUD with diff viewer and auto-schedule.

## Implementation Order

| ⭐  | Order | Deliverable                                                                         | Depends On                                                        | Status |
| --- | :---: | ----------------------------------------------------------------------------------- | ----------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 GPT partition write -- create/delete/resize + dual-header CRC update             | Existing `gpt_crc32()`, `guid_generate()`, `gpt_sync_backup()`   |  [ ]   |
| 💎  |   2   | §2 MBR partition write -- create/delete, CHS encoding, slot management              | Existing MBR parse + EBR chain                                    |  [ ]   |
| 💎  |   3   | §3 `diskpart` CLI -- interactive: list/create/delete/format/assign/active/info      | §1, §2 (CRUD backend); filesystem formatters for `format` command  |  [ ]   |
| 💎  |   4   | §4 `chkdsk` CLI + GUI -- per-FS validation, repair, progress, boot-schedule         | Per-FS fsck backends in the NTFS / IXFS / FAT32 / exFAT TODOs (future; not a single XREF) |  [ ]   |
| 💎  |   5   | §5 `defrag` CLI + GUI -- fragmentation analysis, block relocation, TRIM            | IXFS defrag backend (TODO-07 §7); FAT32/NTFS defrag pass          |  [ ]   |
| 💎  |   6   | §6 `sfc` CLI + GUI -- build-time manifest, runtime verify, repair                  | Build script extension for manifest generation                    |  [ ]   |
| ⭐  |   7   | §7 `recover` CLI + GUI -- IXFS/FAT32 deleted file recovery + data carving           | IXFS inode scan (TODO-07); FAT32 0xE5 scan                        |  [ ]   |
| 💎  |   8   | §8 Disk Management GUI -- two-panel window, partition bar, context menu, SMART     | §1, §2 (write ops); §3 partial (format); compositor/widget layer  |  [ ]   |
| ⭐  |   9   | §9 `diskuse` CLI + GUI -- recursive size walker, treemap, "Largest Files"           | VFS `readdir`/`stat` working; Win32 file API (TODO-05)            |  [ ]   |
| ⭐  |  10   | §10 Snapshot manager CLI + GUI -- CRUD, diff viewer, auto-schedule                  | IXFS snapshot API (TODO-07 §8); §5 (GUI widget layer established) |  [ ]   |

> §1–§6 are `💎` parity -- standard disk management features present in every OS. §7, §9, §10 are `⭐` exclusive: deleted file recovery in-kernel without a third-party tool, in-kernel treemap disk usage analysis, and a snapshot manager integrated directly into the storage layer are not stock features in Windows 11 or Linux desktop environments.

---

## 1. GPT Partition Write `[Sonnet]`

Add `gpt_partition_create`, `gpt_partition_delete`, and `gpt_partition_resize` to the GPT layer. On every mutation: recalculate the partition entry array CRC, update both the primary and backup GPT headers, and write all three areas atomically.

**Files:** `src/kernel/fs/gpt.c` (extend), `include/kernel/fs/gpt.h` (extend)

> [!NOTE]
> GPT mutation must maintain invariants: (1) primary GPT header at LBA 1; (2) backup GPT header at the last LBA of the disk; (3) both headers contain an identical CRC of the partition entry array (`part_entry_crc32`); (4) each header's own CRC (`header_crc32`) covers the header with the CRC field zeroed. Write order: write the partition entry array sectors first, then write the backup header (at last LBA), then write the primary header (at LBA 1) -- this ensures that on power failure, the backup is always consistent. `guid_generate()` (RDRAND-backed) already exists in `gpt.c` for generating the new partition GUID.

- [ ] `gpt_partition_create(dev, start_lba, end_lba, type_guid, name_utf16, &entry_idx)`: find first free slot (`entry.type_guid == all zeros`); populate `gpt_entry`: `partition_guid = guid_generate()`, `first_lba = start_lba`, `last_lba = end_lba`, `type_guid`, `name[36]` (UTF-16LE, from `name_utf16`); call `gpt_write_all(dev)` → return `entry_idx`
- [ ] `gpt_partition_delete(dev, entry_idx)`: zero out `gpt_entry[entry_idx]`; call `gpt_write_all(dev)`
- [ ] `gpt_partition_resize(dev, entry_idx, new_end_lba)`: validate `new_end_lba < next partition start_lba`; update `entry.last_lba`; call `gpt_write_all(dev)`
- [ ] `gpt_write_all(dev, table)`: (1) write partition entry array (LBA 2, count=128, 512 bytes each) and compute `part_entry_crc32`; (2) update `header.part_entry_crc32`; (3) write backup header (last LBA); (4) write primary header (LBA 1); return 0 or -EIO
- [ ] `gpt_find_free_range(dev, size_lba, &start_lba)`: scan gaps between partitions (sorted by `first_lba`); return first gap ≥ `size_lba` sectors
- [ ] `gpt_partition_set_attribute(dev, entry_idx, attr_bit, value)`: set/clear bits in `entry.attributes` (bit 2 = required, bit 60 = read-only, bit 62 = hidden, bit 63 = no-automount)
- [ ] Commit: `"fs/gpt: partition write -- create/delete/resize, dual-header CRC update, atomic write order"`

## 2. MBR Partition Write `[Sonnet]`

Add `mbr_partition_create` and `mbr_partition_delete` to the MBR layer. Encode CHS geometry. Write the updated MBR sector. Manage the four primary slots and extended partition chains.

**Files:** `src/kernel/fs/mbr.c` (extend), `include/kernel/fs/mbr.h` (extend)

> [!NOTE]
> MBR partition entry (16 bytes each, 4 entries at offset 446): `status (1)`, `chs_first[3]`, `type (1)`, `chs_last[3]`, `lba_start (4, LE)`, `lba_count (4, LE)`. CHS encoding: for LBAs ≥ 8 032 897 sectors (8 GB), CHS fields are saturated to maximum (`0xFE 0xFF 0xFF`) -- LBA mode is always used in practice, CHS is a legacy hint only. For modern disks: always write saturated CHS. `mbr_partition_create` must validate: no overlap with existing partitions; at most 3 primary partitions if creating one (4th slot reserved for extended if needed). Active flag: only one partition may have `status = 0x80`; others must be `0x00`.

- [ ] `mbr_partition_create(dev, start_lba, size_lba, type_byte, &slot)`: find free slot (0–3); check no overlap; write CHS with saturation for large disks; write `lba_start`, `lba_count`; write MBR sector at LBA 0; return `slot`
- [ ] `mbr_partition_delete(dev, slot)`: zero 16 bytes at `MBR[446 + slot*16]`; write MBR sector
- [ ] `mbr_chs_encode(lba, &chs_buf)`: if `lba >= 8032897`: write `0xFE 0xFF 0xFF`; else: `C = lba / (H*S)`, `H = lba % (H*S) / S`, `S = lba % S + 1` using 255 heads / 63 sectors
- [ ] `mbr_set_active(dev, slot)`: set `entries[slot].status = 0x80`; clear `status = 0x00` on all other slots; write MBR
- [ ] `mbr_partition_type_for_fs(fs_name)` → type byte: `"fat32"` → `0x0C` (FAT32 LBA), `"ntfs"` → `0x07`, `"ixfs"` → `0xAB` (custom), `"exfat"` → `0x07` (exFAT uses NTFS type byte conventionally), `"linux"` → `0x83`
- [ ] Commit: `"fs/mbr: partition write -- create/delete, CHS saturation, active flag, MBR sector write"`

## 3. `diskpart` CLI `[Sonnet]`

Interactive disk management shell. Commands: `list disk`, `list part <disk>`, `create part primary size=<mb>`, `delete part`, `format`, `assign letter=<c>`, `active`, `info`. Mandatory confirmation for destructive ops.

**Files:** `src/shell/cmd_diskpart.c` (new), `include/shell/diskpart.h` (new)

> [!NOTE]
> `diskpart` is a stateful interactive CLI: user first selects a disk (`select disk 0`) and partition (`select part 1`); subsequent commands operate on the selected context. Implemented as a sub-shell loop within the main `cmd.exe` command dispatcher. Format: calls the appropriate filesystem formatter (`fat32_format()`, `ntfs_format()`, `ixfs_format()`, `exfat_format()`) after creating the partition. Quick format: skip zero-fill, write only filesystem metadata structures. After any partition CRUD: call `vfs_auto_assign_letters()` (→ XREF: TODO-03 §5) to update drive letter assignments.

- [ ] Sub-shell entry: `diskpart` command → print `DISKPART>` prompt; read lines from stdin; dispatch commands; `exit` returns to main shell
- [ ] `list disk`: enumerate `blkdev_t` devices; print index, model string, size, partition table type (GPT/MBR/RAW), free space
- [ ] `list part [disk]`: print all partitions on selected/specified disk: index, type, start LBA, size, filesystem, assigned letter
- [ ] `select disk N` / `select part N`: set active disk/partition context
- [ ] `create part primary size=<mb> [fs=fat32|ntfs|ixfs|exfat] [label=<str>]`: `gpt_find_free_range()` → `gpt_partition_create()` → if `fs` specified: format with the appropriate formatter
- [ ] `delete part [noerr]`: print "WARNING: This will destroy all data. Type 'YES' to confirm: "; on confirmation: `gpt_partition_delete()` or `mbr_partition_delete()`; `vfs_auto_assign_letters()`
- [ ] `format fs=<type> [label=<str>] [quick]`: format selected partition; `quick` skips zero-fill
- [ ] `assign letter=<c>`: call `vfs_mount()` with specified letter for selected partition
- [ ] `active`: `mbr_set_active()` for MBR disks; sets boot flag on GPT partition (attribute bit 2) for GPT disks
- [ ] `info [drive]`: print volume label, filesystem type, total/free/used blocks, partition GUID, attributes
- [ ] `clean`: write zeros to MBR/GPT headers and partition entry array; print confirmation requirement
- [ ] Commit: `"shell: diskpart CLI -- interactive CRUD, format, assign letter, active flag, confirmation guards"`

## 4. `chkdsk` CLI + GUI `[Sonnet]`

Run per-filesystem validation and optional repair. Real-time progress output. GUI panel: drive selector, Scan/Fix options, progress bar, results panel, schedule-on-next-boot for system drive.

**Files:** `src/shell/cmd_chkdsk.c` (new/extend), `src/desktop/dlg_chkdsk.c` (new)

> [!NOTE]
> `chkdsk` already has per-filesystem backends wired in earlier TODOs: `ntfs_fsck()` (TODO-02 §1), `ixfs_fsck()` (01-boot-platform/TODO-22 §3, shipped), `fat32_fsck()` (TODO-04 §8), `exfat_fsck()` (TODO-08 §10), `ext4_fsck()` (TODO-09 §12). This section adds the unified dispatcher, CLI interface, progress reporting via a callback, and the GUI dialog. Boot-time schedule: write `HKLM\SYSTEM\Storage\Chkdsk\Schedule\{letter}` = 1; at next kernel boot, `kernel_init` reads this key and runs chkdsk before mounting the volume. Validation per filesystem: NTFS: `$Bitmap` vs MFT allocated clusters, `$LogFile` dirty state; IXFS: superblock CRC, free bitmap, inode refcounts, journal; FAT32: BPB fields, cross-linked chains, lost clusters (orphaned FAT chains); exFAT: allocation bitmap vs FAT chain consistency.

- [ ] `chkdsk_dispatch(letter, fix, scan_only, progress_cb, ctx)`: look up filesystem type for `letter`; dispatch to `ntfs_fsck`/`ixfs_fsck`/`fat32_fsck`/`exfat_fsck`/`ext4_fsck`; pass progress callback; return error count
  > **Ready:** `fat32_fsck(vol, fix)` is implemented (TODO-04 §8) with BPB validation, FAT1/FAT2 compare, cross-link detection, and lost cluster detection/recovery.
- [ ] CLI: `chkdsk <drive> [/fix] [/scan] [/schedule]`; `/fix` → `fix=1`; `/schedule` → write registry key and exit; real-time line output via progress callback
- [ ] Boot-time schedule: `kernel_init_storage()` reads `HKLM\SYSTEM\Storage\Chkdsk\Schedule\*`; for each scheduled drive: run chkdsk before `vfs_mount()`; clear registry key after
- [ ] GUI dialog `dlg_chkdsk_open(letter)`: drive selector dropdown; `Scan` / `Fix` radio buttons; `Schedule on next boot` checkbox for system drive; `Start` button; progress bar (0–100%); scrollable results log; `Close` / `Export Log` buttons
- [ ] Progress callback: emit `{phase, percent, message}` triples; GUI receives via message queue; CLI prints to stdout
- [ ] Commit: `"shell: chkdsk CLI + GUI -- per-FS dispatch, progress callback, boot-schedule, results log"`

## 5. `defrag` CLI + GUI `[Opus]`

Fragmentation analysis per filesystem. Journal-safe block relocation engine running on `SCHED_IDLE` thread. TRIM free ranges after compaction. GUI: volume selector, fragmentation %, animated block map, progress + ETA, schedule.

**Files:** `src/shell/cmd_defrag.c` (new), `src/desktop/dlg_defrag.c` (new), `src/kernel/fs/defrag_engine.c` (new)

> [!NOTE]
> Defrag is `[Opus]` because journal-safe block relocation requires careful ordering: (1) allocate a temporary block; (2) copy source data; (3) journal the new extent mapping (point inode extent to temp block); (4) commit journal; (5) free old block -- all inside a JBD2/IXFS-WAL transaction. On crash between steps, the journal ensures the extent points to the copy. For NTFS: use `FSCTL_MOVE_FILE` ioctl (§10 of TODO-05). For IXFS: `FSCTL_DEFRAGMENT_FILE` (TODO-07 §7). For FAT32: move clusters by updating FAT chain entries. For exFAT: relocate clusters + update `NoFatChain` flag if coalesced. SCHED_IDLE thread: set thread priority to `THREAD_PRIORITY_IDLE` so defrag never competes with user-visible I/O. TRIM: after defrag, issue `blkdev_discard()` for all free block ranges.

- [ ] `defrag_analyze(letter, &report)`: walk filesystem allocation metadata; compute `fragmented_files`, `total_fragments`, `fragmentation_percent`, `largest_free_run`; fast read-only pass
- [ ] `defrag_engine_run(letter, progress_cb, ctx)`: on `SCHED_IDLE` thread; iterate fragmented files sorted by severity; for each: journal-safe move; yield after each file (check `should_stop` flag); call `progress_cb` after each file
- [ ] `defrag_trim_free(letter)`: enumerate free block runs from filesystem bitmap; issue `blkdev_discard(start, count)` for each; skip if device doesn't support TRIM
- [ ] CLI: `defrag <drive> [/analyze] [/trim] [/pause] [/resume] [/verbose]`; `/analyze` prints report without moving any blocks; real-time per-file progress output
- [ ] GUI dialog `dlg_defrag_open(letter)`: volume list with fragmentation % column; animated 256-color block map (blue=used, white=free, red=fragmented, green=in-progress); `Analyze` / `Defragment` / `Trim` buttons; stop button; progress bar with ETA; schedule weekly/monthly picker
- [ ] Defrag schedule: write `HKLM\SYSTEM\Storage\Defrag\Schedule\{letter}` with `{interval_days, last_run}`; kernel scheduler wakes the defrag thread on schedule
- [ ] Commit: `"shell: defrag CLI + GUI -- journal-safe relocation, TRIM, SCHED_IDLE thread, animated block map"`

## 6. `sfc` CLI + GUI `[Sonnet]`

System File Checker. Build-time: generate a manifest of system files (path, CRC32C, size, version). Runtime: compare live files against manifest; repair from recovery image. GUI: Scan Now, progress bar, results table, log export.

**Files:** `src/shell/cmd_sfc.c` (new), `src/desktop/dlg_sfc.c` (new), `scripts/gen_sfc_manifest.sh` (new)

> [!NOTE]
> Build-time manifest: `scripts/gen_sfc_manifest.sh` runs during `bash scripts/build.sh`; walks `build/sysroot/C:/Impossible/System32/` and `build/sysroot/C:/Impossible/System/`; for each file: computes CRC32C + size + version string (from PE optional header or ELF note); writes `build/sfc_manifest.bin` (binary: `{path_len(2), path[path_len], crc32c(4), size(8), version(4)}`); embeds `sfc_manifest.bin` into the disk image at `C:\Impossible\System\sfc_manifest.bin`. Recovery image: `C:\Impossible\System\recovery.img` (small compressed archive of system files from build); `sfc_repair()` mounts it and copies verified originals. The runtime checker uses `gpt_crc32()` for CRC32C (same polynomial as the filesystem checkers).

- [ ] `scripts/gen_sfc_manifest.sh`: walk build sysroot; CRC32C each file; write binary manifest; emit count of scanned files to stdout
- [ ] `Makefile` integration: add `gen_sfc_manifest` step before disk image assembly; embed `sfc_manifest.bin` into IXFS partition
- [ ] `sfc_scan(progress_cb, &results)`: read `C:\Impossible\System\sfc_manifest.bin`; for each entry: open file, compute CRC32C, compare; collect mismatches into `sfc_result_t[]`; return mismatch count
- [ ] `sfc_repair(results, count)`: for each mismatch: extract original from `recovery.img`; overwrite; verify CRC32C after repair; log `[SFC] Repaired %s`
- [ ] CLI: `sfc [/scannow] [/verifyonly] [/scanonce] [/log=<path>]`; `/scannow` → scan + repair; `/verifyonly` → report only; real-time per-file progress; write detailed log to `X:\Logs\CBS.log`
- [ ] GUI dialog `dlg_sfc_open()`: `Scan Now` / `Verify Only` buttons; progress bar; results table (path, status: OK/MODIFIED/MISSING/REPAIRED); `Export Log` button; `Close`
- [ ] Commit: `"shell: sfc CLI + GUI -- build-time CRC manifest, runtime scan+repair, recovery image, CBS.log"`

## 7. `recover` CLI + GUI `[Opus]`

Deleted file recovery: IXFS (scan inode table for `i_links == 0`), FAT32 (scan `0xE5` entries), data carving (JPEG/PNG/PDF/ZIP signatures). Recovery Wizard GUI: select drive → scan → results table → select → choose destination.

**Files:** `src/shell/cmd_recover.c` (new), `src/desktop/dlg_recover.c` (new)

> [!NOTE]
> This is `[Opus]` because data carving is a novel feature with no prior Impossible OS precedent -- it requires scanning raw block data for file header signatures without filesystem metadata, maintaining a state machine for each file type, handling fragmented files (best-effort), and avoiding false positives from partial overwrites. IXFS recovery: walk the inode table; for each inode where `i_links == 0` and extent chain is still intact (blocks not yet reallocated): reconstruct the file. FAT32 recovery: scan directory entries for `0xE5` first byte; reconstruct the filename (first char lost); follow the FAT chain from the stored start cluster (may be partially overwritten). Data carving: read all unallocated sectors; scan for: JPEG (`FF D8 FF`), PNG (`89 50 4E 47`), PDF (`25 50 44 46`), ZIP (`50 4B 03 04`); carve until end-of-file marker or max size.

- [ ] `recover_scan_ixfs(vol, &results, progress_cb)`: walk inode table; `i_links == 0`: check extent chain validity; compute recovery confidence (100% if extents intact, 0% if blocks reallocated)
- [ ] `recover_scan_fat32(vol, &results, progress_cb)`: walk all directory sectors for `0xE5` entries; recover name (first char → `?`); follow FAT chain; confidence = % of chain blocks not yet reallocated
- [ ] `recover_carve(vol, &results, progress_cb)`: enumerate unallocated blocks from filesystem bitmap; scan for known file signatures; carve runs; assign type + estimated size; confidence = % of carved data sequential
- [ ] `recover_extract(result, dst_path)`: copy recovered data to `dst_path`; return 0 or partial-byte count
- [ ] CLI: `recover <drive> [/scan] [/carve] [/dest=<dir>] [/type=ixfs|fat32|carve]`; prints results table; interactive file selection; writes to destination
- [ ] GUI wizard `dlg_recover_open()`: page 1: drive selector + scan options (IXFS / FAT32 / Data Carving checkboxes); page 2: scan progress; page 3: results table (filename/path, size, type, confidence %, recoverable?); multi-select; page 4: destination picker + `Recover` button; page 5: per-file success/failure summary
- [ ] Commit: `"shell: recover CLI + GUI -- IXFS inode scan, FAT32 0xE5, data carving JPEG/PNG/PDF/ZIP, wizard"`

## 8. Disk Management GUI `[Sonnet]`

Two-panel window: upper table (drives + partitions + health), lower graphical partition bar (proportional colored segments). Partition context menu (create/delete/format/assign letter). SMART status panel.

**Files:** `src/desktop/dlg_diskmgmt.c` (new), `include/desktop/dlg_diskmgmt.h` (new)

> [!NOTE]
> Lower panel: for each disk, draw a horizontal bar scaled to total disk size; each partition is a colored rectangle proportional to its size; label inside rectangle = drive letter + filesystem + size; colors: FAT32=orange, NTFS=blue, IXFS=purple, exFAT=teal, unallocated=dark gray. SMART: read SMART attributes via `atapi_dma_command()` / ATA SMART READ DATA command (CDB: `0xB0 0xD0 LBA_MID=0x4F LBA_HIGH=0xC2`); display Reallocated Sectors, Pending Sectors, Uncorrectable Sectors, Temperature, Power-On Hours in a side panel. Context menu per partition: `Format...`, `Delete Partition`, `Assign Drive Letter`, `Change Label`, `Properties`. Context menu on unallocated space: `New Simple Volume` → opens a wizard.

- [ ] `diskmgmt_enumerate(disk_list_out)`: call `blkdev_enumerate()`; for each: parse GPT/MBR; build `diskmgmt_disk_t { model, total_lba, partitions[], smart_attrs[] }`
- [ ] Upper table: `WM_PAINT` → draw `ListView`-style rows: disk index, model, total size, health icon (green/yellow/red from SMART); under each disk row: partition sub-rows with letter, fs, size, used%, status
- [ ] Lower bar: `WM_PAINT` → for each disk: draw proportional rectangles; color-coded by filesystem; labels with size; hover tooltip shows full partition info
- [ ] Context menu: right-click on partition rect → pop-up menu; `Format...` → open `dlg_format_wizard`; `Delete` → confirm dialog → `gpt_partition_delete()`; `Assign Letter` → input dialog → `vfs_mount()`
- [ ] SMART panel: side panel shows attribute table; Reallocated Sectors > 0 → red; Temperature > 55°C → yellow; Power-On Hours for wear indicator
- [ ] `New Simple Volume` wizard: size slider (min 8 MiB, max free space); filesystem picker; label; drive letter; creates partition + formats
- [ ] Commit: `"desktop: Disk Management GUI -- partition bar, SMART panel, context menus, New Simple Volume wizard"`

## 9. `diskuse` CLI + GUI `[Sonnet]`

Recursive directory size walker. CLI tree output. GUI treemap (rectangles sized by space). "Largest Files" tab. Right-click Open / Delete / Properties on any item.

**Files:** `src/shell/cmd_diskuse.c` (new), `src/desktop/dlg_diskuse.c` (new)

> [!NOTE]
> Treemap algorithm: squarified treemap -- recursively divide the rectangle proportional to subdirectory sizes; assign a color per top-level subdirectory (hue rotated); label each cell when wide enough (> 40 px). Walker: BFS over the VFS using `vfs_readdir()` + `vfs_stat()`; accumulate `total_bytes` per directory; sort children by size descending; stop at symlinks to avoid cycles. CLI output format: similar to `du -sh --max-depth=2` on Linux; column-aligned tree with size + percentage of parent.

- [ ] `diskuse_walk(path, max_depth, &node_tree, progress_cb)`: BFS via `vfs_readdir`/`vfs_stat`; build `du_node_t { name[256], path[1024], size_bytes, child_count, children[] }`; follow `GetFileSizeEx` for files; skip mountpoints and symlinks to avoid loops
- [ ] CLI: `diskuse <path> [/depth=N] [/sort=size|name] [/top=N]`; default depth = 2; tree output with size prefixes; `/top=N` prints largest N files flat
- [ ] GUI `dlg_diskuse_open(path)`: header: drive letter + total/used/free; tab 1 "Treemap": squarified rectangles; click → drill in; breadcrumb nav; hover tooltip with size + %; tab 2 "Largest Files": sortable table (name, path, size, last modified); tab 3 "Tree": collapsible directory tree with size column; right-click on any item: `Open`, `Delete`, `Properties`
- [ ] Delete from diskuse: `DeleteFileW` / `RemoveDirectoryW` → refresh treemap
- [ ] Export: `Save Report...` → writes a flat CSV of `path, size_bytes, is_dir`
- [ ] Commit: `"shell: diskuse CLI + GUI -- recursive walker, squarified treemap, largest files, right-click actions"`

## 10. Snapshot Manager CLI + GUI `[Sonnet]`

CRUD wrapper over the IXFS snapshot API. CLI: `snapshot create|list|restore|delete|diff <drive> [name]`. GUI: chronological timeline, diff viewer (added/modified/deleted), auto-schedule.

**Files:** `src/shell/cmd_snapshot.c` (new), `src/desktop/dlg_snapshot.c` (new)

> [!NOTE]
> Backend: `ixfs_snapshot_create()`, `ixfs_snapshot_list()`, `ixfs_snapshot_restore()`, `ixfs_snapshot_delete()`, `ixfs_snapshot_diff()` from `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §8`. `snapshot diff` returns a list of `{path, change_type (ADD/MOD/DEL), old_size, new_size}` tuples by comparing inode generation numbers between the snapshot's B-tree and the live filesystem. CLI: plain table output. GUI timeline: horizontal scrollable timeline; each snapshot = labeled dot with creation time and name; click → show diff in right panel; Create (names from timestamp by default) / Restore (requires confirmation) / Delete buttons. Auto-schedule: `HKLM\SYSTEM\Storage\Snapshots\Schedule\{letter}` registry key = `{interval_hours, retain_count, last_snapshot_xid}`; a kernel timer fires the snapshot thread.

- [ ] CLI: `snapshot create <drive> [name]` → `ixfs_snapshot_create(letter, name)` → print snapshot ID; `snapshot list <drive>` → print table (ID, name, creation time, size on disk); `snapshot restore <drive> <id>` → confirm + `ixfs_snapshot_restore()`; `snapshot delete <drive> <id>` → `ixfs_snapshot_delete()`; `snapshot diff <drive> <id1> [id2]` → print diff table
- [ ] GUI `dlg_snapshot_open(letter)`: left panel: timeline of snapshots with creation times; right panel: diff tree (ADD=green, MOD=yellow, DEL=red) for selected snapshot vs live (or selected vs selected); `Create` button (with optional name input); `Restore` button (with full-volume confirmation dialog); `Delete` button; status bar showing disk space used by all snapshots
- [ ] Auto-schedule: `snapshot_schedule_check(letter)`: if `time_since_last_snapshot > interval_hours`: call `ixfs_snapshot_create()`; prune oldest if `count > retain_count`; write `last_snapshot_xid` to registry
- [ ] `snapshot_format_size(bytes)` → human-readable string (B/KiB/MiB/GiB) for timeline labels
- [ ] Commit: `"shell: snapshot manager CLI + GUI -- IXFS snapshot CRUD, diff viewer, auto-schedule, timeline UI"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | GPT partition write                      | ✅ `diskpart.exe`; kernel-level GPT write via | ✅ `fdisk`/`gdisk`/`parted`; GPT write via kernel | ⬜ §1 -- `gpt_partition_create/delete/resize` + atomic backup-first write |
| 💎  | MBR partition write -- CHS saturation, active flag | ✅ `diskpart.exe`; `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` | ✅ `fdisk`; direct MBR sector write      | ⬜ §2 -- saturated CHS for large disks   |
| 💎  | `diskpart` interactive CLI               | ✅ `diskpart.exe`; full-featured; Win11-identical command set | ✅ `fdisk`, `parted`, `mkfs.*` (separate tools, | ⬜ §3 -- unified interactive sub-shell matching Win11 |
| 💎  | `chkdsk` per-filesystem validation + repair + boot-schedule | ✅ `chkdsk.exe`; per-FS (NTFS, FAT32, exFAT); | ✅ `fsck` (separate per-FS tools); no    | ⬜ §4 -- unified dispatcher; GUI with boot-schedule |
| 💎  | `defrag` + fragmentation analysis + TRIM + schedule | ✅ `defrag.exe` / Optimize Drives GUI;   | ✅ `e4defrag`, `btrfs fi defragment`; no | ⬜ §5 -- journal-safe relocation; SCHED_IDLE thread; animated |
| 💎  | `sfc` System File Checker                | ✅ `sfc /scannow`; WFP manifest embedded | ✅ `debsums` / `rpm -V` (package-manager-level, | ⬜ §6 -- build-time CRC manifest; in-kernel verify |
| ⭐  | In-kernel deleted file recovery          | ❌ No built-in recovery; requires third-party | ❌ No built-in recovery; `testdisk`/`photorec` are | ⬜ §7 -- IXFS inode table scan +         |
| 💎  | Disk Management GUI                      | ✅ `diskmgmt.msc`; proportional partition bar; right-click | ✅ `gnome-disks`, `gparted`; SMART via `smartmontools` | ⬜ §8 -- two-panel; color-coded proportional bar; in-kernel |
| ⭐  | `diskuse` squarified treemap             | ❌ No built-in treemap; `WinDirStat` /   | ❌ `du` CLI only; `baobab` is            | ⬜ §9 -- first-class in-kernel recursive walker; squarified |
| ⭐  | Snapshot manager GUI                     | ✅ VSS Shadow Copies GUI (Previous       | ✅ Btrfs snapshots via `snapper`; no     | ⬜ §10 -- IXFS-native; timeline GUI; file-level diff |

> **After §1–§10:** Impossible OS delivers a storage management suite that exceeds Windows 11 in three areas: (1) built-in deleted file recovery without third-party tools (§7); (2) a first-class in-kernel disk usage treemap GUI (§9); (3) a snapshot manager with file-level diff visibility integrated directly into the FS layer (§10). The `diskpart`/`chkdsk`/`defrag`/`sfc` commands achieve full parity with Windows 11 equivalents in a unified native implementation.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] GPT write: `gpt_partition_create(dev, 2048, 1048576, GPT_GUID_MICROSOFT_BASIC_DATA, L"Test", &idx)` → read GPT back; `gpt_parse().entries[idx].first_lba == 2048`; both primary and backup headers have matching CRCs
- [ ] GPT delete: `gpt_partition_delete(dev, idx)` → `gpt_parse().entries[idx].type_guid == {0}`; backup header updated
- [ ] MBR write: `mbr_partition_create(dev, 63, 204800, 0x0C, &slot)` → read MBR sector; entry at `slot` has correct LBA values; CHS saturated correctly for large-disk LBA
- [ ] `diskpart` list: `diskpart` → `list disk` → shows all detected block devices with size; `select disk 0` → `list part 0` → shows all partitions
- [ ] `diskpart` create/format: `create part primary size=100 fs=fat32 label=TEST` → partition appears in `list part`; `assign letter=F` → `dir F:\` works; `delete part` (with YES confirmation) → partition gone
- [ ] `chkdsk`: `chkdsk C:` → `[chkdsk] 0 errors found` on clean IXFS volume; inject a bitmap inconsistency → `chkdsk C: /fix` repairs it; `chkdsk C: /schedule` → `HKLM\SYSTEM\Storage\Chkdsk\Schedule\C` = 1
- [ ] `defrag`: `defrag D: /analyze` → prints fragmentation % for FAT32 volume; `defrag D:` → moves fragmented files; TRIM pass on SSD-backed volume; GUI block map animates during defrag
- [ ] `sfc`: `sfc /scannow` → reads manifest; all system files match; modify one system file; `sfc /scannow` → detects mismatch; `sfc /scannow` (repair) → file restored from recovery image; `CBS.log` contains entry
- [ ] `recover`: `recover D: /scan` after deleting files on FAT32 volume → finds deleted entries; `recover D: /dest=E:\recovered` → files extracted; GUI wizard walks through all pages correctly
- [ ] Disk Management GUI: opens with all disks + proportional partition bars; right-click partition → Format → selects FS → formats; SMART temperature reads from AHCI SMART READ DATA command
- [ ] `diskuse C:\Impossible` → tree with sizes; GUI treemap shows proportional rectangles; "Largest Files" tab sorts by size; right-click + Delete on a file → file removed + treemap refreshes
- [ ] Snapshot manager: `snapshot create C: before-update` → listed in `snapshot list C:`; modify files; `snapshot diff C: <id>` → diff shows changed files; GUI timeline shows snapshot; Restore with confirmation works
- [ ] Commit: `"shell: complete storage tool suite -- GPT/MBR write, diskpart, chkdsk, defrag, sfc, recover, diskmgmt, diskuse, snapshot"`
