---
description: Complete FAT32 filesystem test workflow using test disk images
---

# FAT32 Test Workflow

// turbo-all

> Tests the FAT32 driver by attaching a pre-built FAT32 test disk on AHCI port 1.
> The kernel should detect the disk, parse the partition table, mount the FAT32
> filesystem, and allow reading/writing files via VFS.

## Prerequisites

Ensure test disks exist (generated once, cached for future runs):

```bash
make test-disks
```

## 1. Build & Launch with FAT32 Test Disk

1. Build the OS:
```bash
make clean && make all
```

2. Launch QEMU with FAT32 test disk on AHCI port 1:
```bash
make run-test DISK=fat32
```

## 2. Expected Boot Log

Watch serial output for these lines confirming the test disk is detected:

```
  Disk 0, Partition 1: FAT32, 256 MiB (EFI System)
  Disk 0, Partition 2: IXFS, 254 MiB (IXFS)
  Disk 1, Partition 1: FAT32, 8 MiB              ← test disk
[OK] VFS: mounted "FAT32" at D:\                  ← auto-mounted
```

**Key checks:**
- Disk 1 appears (AHCI port 1)
- FAT32 partition detected on test disk
- Mounted to a drive letter (D:\ or E:\)

## 3. Shell Verification Commands

Once the shell is running, test FAT32 read operations:

```
dir D:\                    # List root directory
type D:\test.txt           # Read test file: "Hello from FAT32 test disk!"
dir D:\subdir              # List subdirectory
type D:\subdir\nested.txt  # Read nested file
dir D:\                    # Verify empty.txt and large.bin exist
```

**Expected test disk contents** (populated by `make-test-disks.sh`):

| File | Size | Content |
|------|------|---------|
| `test.txt` | ~30 B | "Hello from FAT32 test disk!" |
| `empty.txt` | 0 B | (empty file) |
| `large.bin` | 64 KiB | Random data |
| `subdir/nested.txt` | ~25 B | "Nested file in subdirectory" |

## 4. Write Verification (if FAT32 write support enabled)

Test creating, writing, and deleting files on the test disk:

```
echo D:\write_test.txt "FAT32 write works!"
type D:\write_test.txt
del D:\write_test.txt
dir D:\
```

## 5. Persistence Test

Test that writes survive a reboot:

1. Write a file: `echo D:\persist.txt "survives reboot"`
2. Exit QEMU (close window or `shutdown`)
3. Re-launch: `make run-test DISK=fat32`
4. Verify: `type D:\persist.txt` should show "survives reboot"

## 6. Edge Cases

Test with different FAT32 scenarios:

```bash
# Long filenames (LFN support)
# Files in deeply nested directories
# Files larger than one cluster (> 4 KiB for 8 MiB disk)
```

## 7. Regenerate Test Disk

If you need a fresh FAT32 test disk (e.g., after corruption testing):

```bash
rm -f build/test-disks/fat32.img
make test-disks
```

## Other Test Disks

The same workflow applies to any filesystem — just change `DISK=`:

```bash
make run-test DISK=ext2          # ext2 test disk
make run-test DISK=ntfs          # NTFS test disk
make run-test DISK=exfat         # exFAT test disk
make run-test DISK=optical/iso9660  # ISO 9660 CD-ROM
```

## Troubleshooting

- **Disk 1 not detected:** Check QEMU output for AHCI port 1 — verify `fat32.img` exists in `build/test-disks/`
- **FAT32 not recognized:** Verify BPB parsing — check `fat32_probe()` in `partition.c`
- **Files not visible:** Check cluster chain traversal — enable serial debug in `fat32.c`
- **Write fails:** Verify FAT write-back to disk — check `fat32_flush()` is called
- **Test disk missing:** Run `make test-disks` to regenerate all images
