<!-- docs: covers=todo/14-host-tools/TODO-02-ixfs-mount.md sources=sdk/src/ixfs-mount,sdk/scripts/mount-ixfs-usb.sh,include/kernel/fs/ixfs.h,tools/make-system-disk.c reviewed=2026-09-30 order=2 -->
# IXFS Mount (Linux)

## What is it?

`ixfs-mount` mounts an IXFS partition from a disk image or a USB drive as an ordinary Linux directory through libfuse3, so a developer can browse and copy Impossible OS files without booting the OS. It is an SDK tool built by [`sdk/build.sh`](sdk-build-system.md), with its own copy of the IXFS parser that uses host types instead of kernel headers. The first five roadmap sections are marked shipped, but **the tool no longer reads images made by the current kernel correctly**, and its write path does not maintain the metadata the kernel now checks. Read [What is not implemented yet?](#what-is-not-implemented-yet) before mounting anything you care about.

## How does it work?

The tool is four layers in [`sdk/src/ixfs-mount/`](../../sdk/src/ixfs-mount/):

- **Disk layer** ([`ixfs-disk.c`](../../sdk/src/ixfs-mount/ixfs-disk.c)): opens the image or device, reads the GPT and finds the partition by its 1-based index. Index 0 treats the whole file as a bare IXFS volume.
- **Core parser** ([`ixfs-core.c`](../../sdk/src/ixfs-mount/ixfs-core.c)): superblock, inode table, inline extents, directory entries, the block bitmap and the write operations (create, write, delete, rename, flush).
- **On-disk structures** ([`ixfs-structs.h`](../../sdk/src/ixfs-mount/ixfs-structs.h)): a hand-maintained copy of [`include/kernel/fs/ixfs.h`](../../include/kernel/fs/ixfs.h) using `<stdint.h>`.
- **FUSE front end** ([`ixfs-fuse-linux.c`](../../sdk/src/ixfs-mount/ixfs-fuse-linux.c)): maps `getattr`, `readdir`, `open`, `read`, `write`, `create`, `mkdir`, `unlink`, `rmdir`, `rename` and `truncate` onto the core. It mounts read-write when the block bitmap loads and read-only otherwise, and always passes `-o default_permissions`.

```mermaid
flowchart LR
    I["image.img:3 or /dev/sdb:3"] --> D[ixfs-disk: GPT lookup]
    D --> C[ixfs-core: superblock, inodes, extents]
    C --> F[ixfs-fuse-linux: FUSE callbacks]
    F --> M["/mnt/ixfs"]
```

The USB helper [`sdk/scripts/mount-ixfs-usb.sh`](../../sdk/scripts/mount-ixfs-usb.sh) walks `/sys/block/sd*` and `/sys/block/nvme*`, keeps USB or removable devices, reads the first four bytes of each partition looking for the IXFS magic `0x49584653`, and mounts the first match at `/mnt/ixfs`.

## What are its interfaces?

| Interface | Notes |
| --- | --- |
| `ixfs-mount <image-or-device>:<partition> <mountpoint> [FUSE options]` | Extra arguments are passed to FUSE |
| `fusermount -u <mountpoint>` | Unmount; the tool then writes back the block bitmap and superblock if anything changed |
| `sdk/scripts/mount-ixfs-usb.sh [/dev/sdX]` | Auto-detect or name the USB device |
| `ixfs_open`, `ixfs_read_inode`, `ixfs_readdir`, `ixfs_lookup`, `ixfs_read_data`, `ixfs_write_data`, `ixfs_create`, `ixfs_delete`, `ixfs_rename`, `ixfs_flush` | The core API in [`ixfs-core.h`](../../sdk/src/ixfs-mount/ixfs-core.h), meant to be shared with the planned disk-inspect and fsck tools |
| `test_ixfs_core <image> <partition>` | Development check built with `make test` in the tool directory |

## How do I use it?

Build it with libfuse3 installed, then pick the right partition. The default image made by [`tools/make-system-disk.c`](../../tools/make-system-disk.c) has the EFI system partition first, the BlackBox FAT32 partition second and IXFS third; the A/B layout adds a second IXFS slot. The roadmap's examples still say `:2`, which is now the BlackBox partition.

The tool mounts read-write whenever it can load the block bitmap, so always pass `-o ro` and work on a copy until the format fix below lands:

```bash
sudo apt install libfuse3-dev
bash sdk/build.sh
cp build/system-disk.img /tmp/inspect.img
mkdir -p /tmp/ixfs
sdk/tools/ixfs-mount /tmp/inspect.img:3 /tmp/ixfs -o ro
ls /tmp/ixfs
fusermount -u /tmp/ixfs
```

Today `ls` shows an empty root, for the reason below.

The development check shows what the parser sees. Against the image built on 2026-09-30 it finds the volume but not its contents:

```text
Disk: partition 3 at offset 203423744 (282066944 bytes)
Sector 0 magic: 0x49584653 (IXFS OK)
IXFS: "Impossible OS" v2, 68864 blocks (63987 free)

ROOT directory (inode 1):

Lookup test: Impossible/ not found in root
```

## What is not implemented yet?

All of the following are filed in section 6 of the roadmap, [Resync the SDK Parser with the IXFS v2 On-Disk Format](../../todo/14-host-tools/TODO-02-ixfs-mount.md#6-resync-the-sdk-parser-with-the-ixfs-v2-on-disk-format).

- **The inode size is wrong.** The kernel's `struct ixfs_inode` is 128 bytes, 32 per block, pinned by a `_Static_assert` in `ixfs.h`; the SDK copy has no `i_reserved` padding and is 92 bytes. Every inode past number 0 is read from the wrong offset, which is why the root directory above lists nothing. The kernel was padded in commit `161d194bd` and the copy was never updated.
- **Writes skip metadata the kernel checks.** The kernel keeps a CRC32C table for data blocks, a write-ahead journal, block refcounts and snapshots (see [IXFS Advanced Features](../storage/ixfs-advanced.md)); the SDK write path updates none of them. Do not mount a real image read-write.
- **Rename stays in one directory.** `ixfs_fuse_rename` resolves only the source's parent, so moving a file into another directory renames it inside its old one.
- **Truncate is partial.** Truncating to zero reports success without freeing blocks; any other size returns `ENOSYS`.
- **No Windows version.** The domain index once described a WinFsp driver; none exists.
- **Nothing tests it.** No suite or workflow builds the tool or runs `test_ixfs_core` against a fresh image; see [SDK Build System](sdk-build-system.md#what-is-not-implemented-yet).

## How does it compare with Windows 11 and Linux?

Linux reaches foreign filesystems through FUSE drivers such as `ntfs-3g` and `ext2fsd` fills the same role on Windows; both need a separate loop device or partition step for an image. `ixfs-mount` takes `image:partition` in one argument and the USB script finds the partition by its magic. Once the format drift is fixed, that is a smaller workflow than either, but today the host-side reader is behind the kernel's format.

## See also

- [IXFS Mount roadmap](../../todo/14-host-tools/TODO-02-ixfs-mount.md)
- [IXFS Core Filesystem](../storage/ixfs-core.md)
- [IXFS Advanced Features](../storage/ixfs-advanced.md)
- [IXFS Consistency Checker](ixfs-fsck.md) and [Disk Image Inspector](disk-inspect.md), which plan to reuse this parser
