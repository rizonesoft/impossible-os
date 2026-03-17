---
description: Complete FAT32 filesystem test workflow using test disk images
---

# FAT32 Test Workflow

// turbo-all

> Builds the OS, launches QEMU with the FAT32 test disk on AHCI port 1,
> and leaves QEMU running for manual shell testing.
>
> **Note:** `make run-test` is an exception to the "always use build.sh" rule
> because `build.sh` does not wrap the `run-test` target. Use raw `make` here only.
>
> Drive letter mapping:
>   C:\ = IXFS (system partition)
>   D:\ = FAT32 (test disk, first non-EFI FAT32 partition)
>   EFI partition = hidden (no drive letter, like Windows)

## Steps

1. Full clean build first (uses build.sh per rules.md):
```bash
bash scripts/build.sh clean
```

2. Verify build succeeded:
```bash
tail -1 build/build.log
```
Expected: `=== BUILD OK ===`

3. Generate test disks and launch QEMU with FAT32 test disk:
```bash
make run-test DISK=fat32
```

> **Why raw `make` here?** `build.sh` does not support the `run-test` target.
> The build itself was already done in step 1 via `build.sh`, so this only
> generates test disks and launches QEMU.

## Expected Serial Output

Watch the terminal for these lines confirming the test disk is detected:

```
[OK] AHCI port 0: "QEMU HARDDISK" (512 MiB, ...)
[OK] AHCI port 1: "QEMU HARDDISK" (8 MiB, ...)
[GPT] sata0: 2 partition(s)
  Disk 0, Partition 1: FAT32, 256 MiB (EFI System)
  Disk 0, Partition 2: IXFS, 254 MiB (IXFS)
[RAW] sata1: no partition table, raw FAT32 volume
  Disk 1, Partition 1: FAT32, 8 MiB (Raw Volume)
[OK] VFS: mounted "IXFS" at C:\
[OK] VFS: mounted "FAT32" at D:\
```

**Key checks:**
- AHCI detects 2 drives (port 0 + port 1)
- sata1 recognized as raw FAT32 volume (no partition table)
- Test disk mounted at D:\ (EFI partition is hidden)

## Shell Verification Commands

Once the desktop appears, switch to the QEMU window shell:

```
dir D:\                    # List FAT32 test disk root
type D:\test.txt           # Should print: "Hello from FAT32 test disk!"
dir D:\subdir              # List subdirectory
type D:\subdir\nested.txt  # Should print: "Nested file in subdirectory"
```

## Troubleshooting

- **Disk 1 not detected:** Verify `build/test-disks/fat32.img` exists (run `make test-disks`)
- **FAT32 not mounted:** Check `partition.c` super-floppy probe — raw FAT32 detection
- **Regenerate test disk:** `rm -f build/test-disks/fat32.img && make test-disks`
