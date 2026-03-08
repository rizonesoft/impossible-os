# Build System

The root `Makefile` handles the entire build pipeline: cross-compilation, linking,
GRUB EFI binary, IXFS system disk, ISO packaging, and QEMU launch.

## Make Targets

| Target | Description |
|--------|-------------|
| `make all` | Auto-increment build → compile → link → ISO → EFI → system disk |
| `make boot` | Assemble bootloader objects only |
| `make kernel` | Compile and link `kernel.exe` |
| `make iso` | Package everything into `build/os-build.iso` |
| `make grub-efi` | Build standalone GRUB EFI binary (`BOOTX64.EFI`) |
| `make system-disk` | Create IXFS system disk image via `mkfs-ixfs` |
| `make run` | Launch QEMU with serial on stdio |
| `make run-debug` | Launch QEMU paused, GDB on port 1234 |
| `make run-log` | Launch QEMU with serial to `serial.log` |
| `make clean` | Remove all build artifacts |

## Build Pipeline

```
make all
  ├── _increment_build     # .build_number += 1
  ├── iso
  │    ├── kernel
  │    │    ├── ASM sources (.asm → .o via NASM)
  │    │    ├── C sources (.c → .o via x86_64-elf-gcc)
  │    │    └── Link → build/kernel.exe
  │    ├── User programs
  │    │    ├── shell.c → shell.exe (user-mode ELF)
  │    │    └── User libc (crt0.o + libc.a)
  │    ├── Host tools
  │    │    └── tools/jpg2raw → build/tools/jpg2raw
  │    ├── Assets
  │    │    └── JPG → RAW conversion (wallpaper, icons)
  │    └── ISO
  │         └── grub-mkrescue → build/os-build.iso
  ├── grub-efi
  │    └── grub-mkimage → build/tools/BOOTX64.EFI (790 KiB PE32+)
  └── system-disk
       ├── mkfs-ixfs → build/tools/mkfs-ixfs
       └── mkfs-ixfs --populate → build/system-disk.img (32 MiB IXFS v2)
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
[BUILD] #142
[EFI] build/tools/BOOTX64.EFI created (790528 bytes)
[DISK] build/system-disk.img created (32 MiB IXFS v2)
[VERSION] Impossible OS v0.1.0.142 (9bc69c7a)
```

## Output

| Artifact | Path | Description |
|----------|------|-------------|
| Kernel | `build/kernel.exe` | Linked kernel binary (ELF format) |
| GRUB EFI | `build/tools/BOOTX64.EFI` | Standalone GRUB EFI binary (PE32+) |
| System Disk | `build/system-disk.img` | Bootable GPT disk (EFI + IXFS) |
| ISO | `build/os-build.iso` | Bootable UEFI ISO (GRUB + Multiboot2, fallback) |

> The primary boot artifact is `system-disk.img`. The ISO is kept as a
> fallback for environments that don't support raw disk images.

## Host Tools

| Tool | Source | Output | Purpose |
|------|--------|--------|---------|
| `jpg2raw` | `tools/jpg2raw.c` | `build/tools/jpg2raw` | Convert JPG/PNG → raw BGRA |
| `mkfs-ixfs` | `tools/mkfs-ixfs.c` | `build/tools/mkfs-ixfs` | Create IXFS v2 disk images |
| `make-system-disk` | `tools/make-system-disk.c` | `build/tools/make-system-disk` | Create GPT system disk |
| `make-test-disks` | `tools/make-test-disks.sh` | `build/test-disks/*.img` | Generate FS test images |
