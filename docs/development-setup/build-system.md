# Build System

The root `Makefile` handles the entire build pipeline: cross-compilation, linking,
GRUB EFI binary, IXFS system disk, and QEMU launch.

## Make Targets

| Target | Description |
|--------|-------------|
| `make all` | Build kernel + userland + EFI + system disk (no ISO) |
| `make boot` | Assemble bootloader objects only |
| `make kernel` | Compile and link `kernel.exe` |
| `make host-tools` | Build host utilities (`jpg2raw`) |
| `make sysroot` | Populate `build/sysroot/` with assets and text files |
| `make userland` | Build libc + user programs (`hello.exe`, `shell.exe`) |
| `make grub-efi` | Build standalone GRUB EFI binary (`BOOTX64.EFI`) |
| `make system-disk` | Create bootable GPT system disk (`system-disk.img`) |
| `make iso` | *(optional)* Package into `build/os-build.iso` |
| `make test-disks` | Generate filesystem test images (`build/test-disks/`) |
| `make run` | Launch QEMU (system disk on AHCI) |
| `make run-test DISK=fat32` | Launch QEMU with secondary test disk on AHCI port 1 |
| `make run-debug` | Launch QEMU paused, GDB on port 1234 |
| `make run-log` | Launch QEMU with serial to `serial.log` |
| `make clean` | Remove all build artifacts |

## Build Pipeline

```
make all
  ├── _increment_build       # .build_number += 1
  ├── kernel
  │    ├── ASM sources (.asm → .o via NASM)
  │    ├── C sources (.c → .o via x86_64-elf-gcc)
  │    └── Link → build/kernel.exe
  ├── host-tools
  │    └── tools/jpg2raw → build/tools/jpg2raw
  ├── sysroot (depends on host-tools)
  │    ├── Text files (hello.txt, readme.txt)
  │    └── JPG → RAW conversion (wallpaper, icons)
  ├── userland (depends on sysroot)
  │    ├── User libc (crt0.o + libc.a)
  │    ├── hello.c → hello.exe (user-mode ELF)
  │    └── shell.c → shell.exe (user-mode ELF)
  ├── grub-efi
  │    └── grub-mkimage → build/tools/BOOTX64.EFI
  └── system-disk (depends on kernel + userland + grub-efi)
       ├── make-system-disk → GPT partition table
       ├── mkfs.fat → EFI partition (kernel + GRUB)
       └── mkfs-ixfs → IXFS partition (sysroot files)
```

## Source Discovery

The Makefile uses `find` to automatically discover source files:

```makefile
ASM_SRCS := $(shell find $(BOOT_DIR) $(KERNEL_DIR) -name '*.asm')
C_SRCS   := $(shell find $(KERNEL_DIR) $(LIBC_DIR) $(DESKTOP_DIR) -name '*.c')
```

**No manual Makefile edits required** when adding new `.c` or `.asm` files to
existing directories.

## Compiler Flags

```makefile
CFLAGS := -Wall -Wextra -Werror \
          -ffreestanding -nostdlib -nostdinc \
          -fno-stack-protector -fno-pie -no-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -std=gnu11 -O2 -g \
          -Iinclude -Isrc/kernel
```

Version information is appended automatically:

```makefile
CFLAGS += -DVERSION_MAJOR=$(VERSION_MAJOR) \
          -DVERSION_MINOR=$(VERSION_MINOR) \
          -DVERSION_PATCH=$(VERSION_PATCH) \
          -DVERSION_BUILD=$(BUILD_NUMBER) \
          -DVERSION_GIT_HASH='"$(GIT_HASH)"'
```

## Versioning Integration

| File | Purpose | Tracked in Git? |
|------|---------|----------------|
| `VERSION` | SemVer string (e.g., `0.1.0`) | ✅ Yes |
| `.build_number` | Auto-incrementing counter | ❌ No (gitignored) |

Each `make all` increments `.build_number` and embeds all version info into the
kernel binary, producing output like:

```
[BUILD] #193
[EFI] build/tools/BOOTX64.EFI created (790528 bytes)
[DISK] build/system-disk.img created (512M GPT: EFI + IXFS)
[VERSION] Impossible OS v0.1.0.193 (e0e3f6e1)
```

## Output

| Artifact | Path | Description |
|----------|------|-------------|
| Kernel | `build/kernel.exe` | Linked kernel binary (ELF format) |
| GRUB EFI | `build/tools/BOOTX64.EFI` | Standalone GRUB EFI binary (PE32+) |
| System Disk | `build/system-disk.img` | Bootable 512M GPT disk (EFI + IXFS) |
| ISO | `build/os-build.iso` | *(optional)* Bootable UEFI ISO via `make iso` |
| Test Disks | `build/test-disks/*.img` | FS test images via `make test-disks` |
| Optical | `build/test-disks/optical/*.iso` | ISO 9660, Joliet, UDF test images |

## Host Tools

| Tool | Source | Output | Purpose |
|------|--------|--------|---------|
| `jpg2raw` | `tools/jpg2raw.c` | `build/tools/jpg2raw` | Convert JPG/PNG → raw BGRA |
| `mkfs-ixfs` | `tools/mkfs-ixfs.c` | `build/tools/mkfs-ixfs` | Create IXFS v2 disk images |
| `make-system-disk` | `tools/make-system-disk.c` | `build/tools/make-system-disk` | Create GPT system disk |
| `make-test-disks` | `tools/make-test-disks.sh` | `build/test-disks/` | Generate all FS + optical test images |

## Test Disk Library

`make test-disks` generates pre-formatted disk images for filesystem driver testing:

| Image | Size | Format | Tool |
|-------|------|--------|------|
| `fat32.img` | 8 MiB | FAT32 + sample files | `mkfs.fat` + `mcopy` |
| `exfat.img` | 8 MiB | exFAT | `mkfs.exfat` |
| `ext2.img` | 4 MiB | ext2 + sample files | `mkfs.ext2` + `debugfs` |
| `ext3.img` | 4 MiB | ext3 + journal | `mkfs.ext3` + `debugfs` |
| `ext4.img` | 8 MiB | ext4 + extents | `mkfs.ext4` + `debugfs` |
| `ntfs.img` | 16 MiB | NTFS + sample files | `mkntfs` + `ntfscp` |
| `ixfs.img` | 8 MiB | IXFS v2 | `mkfs-ixfs` |
| `mbr.img` | 8 MiB | MBR partition table | `dd` + manual entries |
| `gpt.img` | 16 MiB | GPT partition table | `dd` + EFI PART header |

Optical images in `build/test-disks/optical/`:

| Image | Format | Tool |
|-------|--------|------|
| `iso9660.iso` | ISO 9660 | `xorriso` or `genisoimage` |
| `joliet.iso` | ISO 9660 + Joliet | `xorriso -J` |
| `udf.iso` | UDF 1.02 | `genisoimage -udf` or `mkudffs` |
| `udf250.iso` | UDF 2.50 | `mkudffs --udfrev=0x0250` |
| `mixed.iso` | ISO 9660 + UDF bridge | `xorriso -J -udf` |

Use `make run-test DISK=<name>` to attach any test disk on AHCI port 1.
