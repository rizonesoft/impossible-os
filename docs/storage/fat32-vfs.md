<!-- docs: covers=todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md sources=src/kernel/fs/fat32/fat32_core.c,src/kernel/fs/fat32/fat32_ops.c,src/kernel/fs/fat32/fat32_dir.c,src/kernel/fs/fat32/fat32_write.c,src/kernel/fs/fat32/fat32_fsck.c,src/kernel/fs/fat32/fat32_internal.h,src/kernel/fs/vfs.c,include/kernel/fs/vfs.h,src/kernel/nt/nt_syscall.c,src/kernel/test/test_ixfs.c,src/kernel/test/test_vfs.c reviewed=2026-09-28 order=4 -->
# FAT32 and VFS Semantics

## What is it?

FAT32 is the read-write filesystem for the BlackBox partition, USB sticks and any FAT32 data disk, and the VFS is the layer every filesystem sits under. This roadmap hardened FAT32 (strict boot-sector checks, free-space recovery, FAT mirror repair, long-name writes, timestamps, a repair pass) and added the Windows file semantics the VFS owes every driver (case-insensitive names, share modes, delete-on-close, byte-range locks, opportunistic locks). Most of its 18 sections are shipped; the repair pass, truncate-on-open and atomic replace are partly done, and two sections are open.

## How does it work?

**Mounting.** `fat32_init()` in [`fat32_ops.c`](../../src/kernel/fs/fat32/fat32_ops.c) reads the boot sector and validates it strictly in [`fat32_core.c`](../../src/kernel/fs/fat32/fat32_core.c): jump instruction, sector size, cluster size, FAT count, root cluster and region sizes, each failure logged as `BPB validation failed: ...` and refused. It then loads FSInfo, rescanning the FAT when the free count is unknown, and compares FAT1 with FAT2 on every mount, rewriting FAT2 where they differ.

**Dirty volumes.** A volume whose clean bit is cleared is repaired by `fat32_fsck()` in [`fat32_fsck.c`](../../src/kernel/fs/fat32/fat32_fsck.c) before it mounts; if errors remain, the mount is refused rather than exposing a damaged volume as writable. The repair pass finds cross-linked and truncated chains and frees lost clusters, but skips freeing when its directory walk was incomplete, because freeing on a partial walk would destroy live data. Only the BlackBox `X:` volume is marked dirty at mount and clean at shutdown today; other FAT32 volumes never have their marker written.

**Names.** New files get long-name entries and a unique short name (`~1` to `~99`) from [`fat32_dir.c`](../../src/kernel/fs/fat32/fat32_dir.c). Long names are written one byte per UTF-16 unit, so names are only correct for ASCII; a non-ASCII name is stored garbled. Lookups compare case-insensitively inside each driver. A directory lists at most 128 entries (`FAT32_MAX_DIR_ENTRIES` in [`fat32_internal.h`](../../src/kernel/fs/fat32/fat32_internal.h)).

**Limits.** Writes and truncates past 4 GiB are refused with `4 GiB file size limit: <name>`. File times are written on create and modify, but `stat` on FAT32 returns zero times, so the encoded times are not visible to callers.

**VFS semantics.** [`vfs.c`](../../src/kernel/fs/vfs.c) enforces share modes per file (up to four open handles each, `VFS_MAX_HANDLES` in [`vfs.h`](../../include/kernel/fs/vfs.h)), unlinks a delete-on-close file on its last close, keeps up to four byte-range locks per file, and grants Level 1 and Level 2 opportunistic locks with break-on-conflict. `vfs_rename_ex()` can replace an existing file in the same directory. Two gaps limit what user programs see: `NtCreateFile` in [`nt_syscall.c`](../../src/kernel/nt/nt_syscall.c) ignores the requested share mode, so user handles always open share-all, and a sharing violation reaches the caller as a plain failure rather than `STATUS_SHARING_VIOLATION`. Byte-range locks are advisory: `vfs_write()` does not check them.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `fat32_init()`, `fat32_get_driver()`, `fat32_run_fsck()`, `fat32_format()` | Mount, driver table, repair and format ([`fat32_ops.c`](../../src/kernel/fs/fat32/fat32_ops.c)) |
| `vfs_open()`, `vfs_close()`, `vfs_rename_ex()`, `vfs_unlink()` | Share checks, delete-on-close and replace ([`vfs.h`](../../include/kernel/fs/vfs.h)) |
| `vfs_lock_file()`, `vfs_unlock_file()` | Byte-range locks, reached from `NtLockFile` and `NtUnlockFile` |
| `vfs_request_oplock()`, `vfs_break_oplock()` | Level 1 and 2 oplocks, not yet reachable from user mode |

## How do I use it?

A FAT32 volume mounts automatically at boot (see [Volume Management](volume-management.md)); the serial log shows `FAT32: "<label>" <n> MiB, <k> sectors/cluster, root cluster <c>`. A dirty volume adds:

```text
auto-fsck: dirty volume detected; running repair before mount
auto-fsck: repair completed; volume now clean
```

`make test-fs` (or `bash scripts/test.sh SUITE=fs`) runs the FAT32 boot-sector suites in [`test_ixfs.c`](../../src/kernel/test/test_ixfs.c) and the VFS suites in [`test_vfs.c`](../../src/kernel/test/test_vfs.c), including truncate-on-open and replace-on-rename. No suite covers share modes, locks, oplocks, long names or the repair pass.

## What is not implemented yet?

- **The rest of the repair pass**: cycle guard, geometry checks and error propagation ([FAT32 fsck](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#6-fat32-fsck)); there is no `chkdsk` command to run it.
- **Truncate-on-open everywhere** ([VFS_O_TRUNC End-to-End](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#15-vfs_o_trunc-end-to-end)) and **a crash-safe replace**: a failure between removing the old file and renaming the new one loses the old file ([VFS Rename Replace-Existing](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#16-vfs-rename-replace-existing----atomic-temp-file-durable-write-primitive)).
- **No disk I/O under the volume spinlock** ([FAT32 Volume-Lock Hold Time](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#17-fat32-volume-lock-hold-time----no-blocking-disk-io-under-the-cli-vol-lock)).
- **Follow-ups filed after shipping**, including long-name validation on lookup and rename keeping the long name ([Post-Ship Follow-Up Backfill](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#18-post-ship-follow-up-backfill-orphan-cohort-2026-07-31)).
- **Share modes and oplocks from user mode**, which wait on the Win32 file layer ([Win32 File I/O](win32-file-io.md)).

## How does it compare with Windows 11 and Linux?

Windows 11's `fastfat` validates strictly, writes long names, checks share modes per file and offers every oplock level; `chkdsk` repairs. Linux's `vfat` writes long names and timestamps, uses POSIX locks and leases, and `fsck.fat` repairs. Impossible OS matches the FAT32 on-disk behaviour for ASCII names and adds automatic repair at mount, but its share-mode and oplock support is not yet reachable from user programs.

## See also

- [FAT32 hardening and VFS semantics roadmap](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md)
- [Volume Management and Auto-mount](volume-management.md)
- [Win32 File I/O](win32-file-io.md)
- [BlackBox Service Partition](../boot/blackbox-service-partition.md)
- [FILETIME and Kernel Time](../kernel/time-filetime.md)
- [Storage](index.md)
