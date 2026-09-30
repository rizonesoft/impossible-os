<!-- docs: covers=todo/01-boot-platform/TODO-22-recovery-partition.md sources=tools/make-system-disk.c,src/kernel/fs/gpt.c,include/kernel/fs/gpt.h,src/kernel/fs/partition.c,include/kernel/fs/partition.h,src/kernel/fs/ixfs/ixfs_fsck.c,include/kernel/fs/ixfs.h,src/kernel/test/test_ixfs_fsck.c,Makefile reviewed=2026-09-30 order=22 -->
# Recovery Partition

## What is it?

The recovery partition is a 34 MiB read-only GPT partition at the end of every A/B system disk, reserved for self-repair without external media. Today it holds one file, a copy of the last-built kernel (`kernel.bak`), and the kernel refuses to mount it during a normal boot. The IXFS filesystem checker that a recovery flow would run already ships, but nothing boots into the recovery partition yet.

In short, this is a partition, not yet an environment. The disk layout, the read-only enforcement and the `kernel.bak` copy are real. The recovery boot path, the kernel recovery-mode entry, kernel restore, boot-metadata reset, NVRAM reconstruction and the recovery UI are all unbuilt. A design review rejected the original plan (a standalone `recovery.efi` at the UEFI fallback path) in favour of booting recovery through the existing bootloader and boot-entry store.

## How does it work?

`tools/make-system-disk.c` builds the GPT for an A/B disk. After the ESP, BlackBox, metadata and slot partitions, it carves the recovery partition at the very end of the disk, and Slot A and Slot B split the space before it. The reserve is 34 MiB rather than 32 MiB because, at one sector per cluster, a 32 MiB FAT32 volume falls below the FAT32 minimum cluster count that UEFI firmware (OVMF) requires; the tool's own comment records the arithmetic.

The partition gets its own type GUID, `49504F53-7265-636F-7665-727900000001`, and its GPT entry sets attribute bit 60, the standard GPT read-only flag. The Makefile's `system-disk` target then formats the region FAT32 with the label `RECOVERY` and copies the freshly built kernel into it as `kernel.bak`.

Read-only is enforced twice. The GPT attribute is the on-disk marker that firmware and partition tools should respect. The kernel enforces it independently: `gpt_is_recovery()` matches the type GUID, the GPT scan in `partition.c` stores the result as `is_recovery`, and `partition_mount_filesystems()` skips any partition with that flag before assigning a drive letter. A normal boot therefore never mounts the recovery volume and never exposes `kernel.bak` to deletion.

The filesystem checker is `ixfs_fsck()` in `ixfs_fsck.c`. It checks the superblock, inode table, free-block bitmap, directory tree, per-block reference counts and the write-ahead journal, and fills a `struct ixfs_fsck_report` with a count per error class. In repair mode it applies only a safe subset: bitmap and free-count reconciliation, reference-count correction, freeing single-owner orphan inodes, clearing bad directory entries, and replaying the journal only after it validates. Cross-linked or snapshot-shared blocks are never freed. It flushes the write-back cache first and repairs through raw disk I/O, so a later cache flush cannot undo a repair. Nothing calls it yet: the unit tests cover its individual validators and the bitmap helper, not the full repair run.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `gpt_is_recovery()` | Matches a partition's type GUID against the recovery GUID ([`gpt.c`](../../src/kernel/fs/gpt.c), [`gpt.h`](../../include/kernel/fs/gpt.h)) |
| `struct partition_info.is_recovery` | Set by the GPT scan and checked before mounting ([`partition.h`](../../include/kernel/fs/partition.h), [`partition.c`](../../src/kernel/fs/partition.c)) |
| `ixfs_fsck(fix, report)` | Runs the IXFS checker, with safe repairs when `fix` is set; no caller yet ([`ixfs.h`](../../include/kernel/fs/ixfs.h), [`ixfs_fsck.c`](../../src/kernel/fs/ixfs/ixfs_fsck.c)) |
| `struct ixfs_fsck_report` | Per-class error tally: orphan inodes, bad entries, cross-links, bitmap, free-count, refcount, journal and checksum faults ([`ixfs.h`](../../include/kernel/fs/ixfs.h)) |
| `make-system-disk --ab` | Writes the GPT, including the recovery entry and its read-only attribute ([`make-system-disk.c`](../../tools/make-system-disk.c)) |
| `system-disk` target | Formats the recovery partition and copies `kernel.bak` into it ([`Makefile`](../../Makefile)) |
| `test_register_ixfs_fsck()` | Registers the checker's helper tests in the `fs` suite ([`test_ixfs_fsck.c`](../../src/kernel/test/test_ixfs_fsck.c)) |

## How do I use it?

The default build creates the partition; there is nothing to enable.

```bash
bash scripts/build.sh
```

The disk-image step prints the layout, including `Part 6 (Recovery): LBA <start> - <end> (34 MiB, read-only)`. There is no way to boot into recovery or to run `ixfs_fsck()` against a real slot yet. The partition's only observable effects are that it exists on disk, holds `kernel.bak`, and never appears as a drive letter.

To run the checker's helper tests, use the filesystem suite:

```bash
bash scripts/test.sh SUITE=fs
```

On Windows, `scripts\debug\kernel\run-fs-tests.bat` runs the same suite.

## What is not implemented yet?

- No boot path enters recovery: nothing dispatches a `BOOT_PATH_RECOVERY` boot into a recovery flow instead of the desktop, and recovery is now planned to boot through the normal bootloader and entry store: [Recovery Bootloader](../../todo/01-boot-platform/TODO-22-recovery-partition.md#2-recovery-bootloader).
- No operation restores `kernel.bak` onto a damaged slot: [Backup Kernel Restore](../../todo/01-boot-platform/TODO-22-recovery-partition.md#4-backup-kernel-restore).
- No operation resets the A/B metadata (tries and successful flags) from recovery: [Boot Metadata Reset](../../todo/01-boot-platform/TODO-22-recovery-partition.md#5-boot-metadata-reset).
- No NVRAM boot-entry reconstruction, including the `PlatformRecovery####` variables firmware consults once `BootOrder` is exhausted: [NVRAM Boot Entry Reconstruction](../../todo/01-boot-platform/TODO-22-recovery-partition.md#6-nvram-boot-entry-reconstruction).
- No recovery UI (status, repair menu, progress): [Recovery UI](../../todo/01-boot-platform/TODO-22-recovery-partition.md#7-recovery-ui).
- The full repair run (orphan freeing, directory repair, journal replay) has no caller and no test that drives it end to end; it arrives with the recovery flow: [Recovery Bootloader](../../todo/01-boot-platform/TODO-22-recovery-partition.md#2-recovery-bootloader).
- Repair mode is not yet safe against read failures: after an unreadable inode the bitmap reconcile can mark live blocks free, an entry whose target inode cannot be read is cleared as dangling, and an unreadable snapshot block is skipped. All three must block repair before recovery calls it: [Gate IXFS fsck Repairs on Complete, Readable Scans](../../todo/01-boot-platform/TODO-22-recovery-partition.md#8-gate-ixfs-fsck-repairs-on-complete-readable-scans).
- Seeding the boot-entry store's recovery entry on first boot belongs to the entry store, not this file: [Bootstrap and First-Install Entry Seeding](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).

## How does it compare with Windows 11 and Linux?

Windows 11 ships a full recovery environment (WinRE) with `chkdsk`, System Restore and `bootrec` for rebuilding boot entries behind a menu. Linux distributions offer an initramfs repair shell with `fsck`, live media for reinstalling a damaged kernel, and `fallback.efi` for lost NVRAM entries, without a unified UI. Impossible OS has the protected partition, a kernel backup and a snapshot-aware filesystem checker, but none of it is reachable from a running recovery environment yet, so it is behind both until the recovery boot path and repair operations land.

## See also

- [Recovery Partition and Self-Repair roadmap](../../todo/01-boot-platform/TODO-22-recovery-partition.md)
- [A/B Dual-Slot Boot and Automatic Rollback](ab-boot-rollback.md)
- [Bootloader Error Recovery](bootloader-error-recovery.md)
- [Boot Entries, Menu and Policy](boot-entries-menu-policy.md)
