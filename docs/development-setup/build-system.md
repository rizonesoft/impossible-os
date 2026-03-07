# Build System

The root `Makefile` handles the entire build pipeline: cross-compilation, linking,
initrd creation, ISO packaging, and QEMU launch.

## Make Targets

| Target | Description |
|--------|-------------|
| `make all` | Auto-increment build number → compile → link → ISO |
| `make boot` | Assemble bootloader objects only |
| `make kernel` | Compile and link `kernel.elf` |
| `make iso` | Package everything into `build/os-build.iso` |
| `make run` | Launch QEMU with serial on stdio |
| `make run-debug` | Launch QEMU paused, GDB on port 1234 |
| `make run-log` | Launch QEMU with serial to `serial.log` |
| `make clean` | Remove all build artifacts |

## Build Pipeline

```
make all
  ├── _increment_build     # .build_number += 1
  └── iso
       ├── kernel
       │    ├── ASM sources (.asm → .o via NASM)
       │    ├── C sources (.c → .o via x86_64-elf-gcc)
       │    └── Link → build/kernel.elf
       ├── User programs
       │    ├── shell.c → shell.exe (user-mode ELF)
       │    └── User libc (crt0.o + libc.a)
       ├── Host tools
       │    └── tools/make-initrd → build/tools/make-initrd
       ├── Assets
       │    └── JPG → RAW conversion (wallpaper, icons)
       ├── Initrd
       │    └── build/initrd.img (hello.txt, shell.exe, wallpaper, etc.)
       └── ISO
            └── grub-mkrescue → build/os-build.iso
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
[BUILD] #42
[ISO] build/os-build.iso created
[VERSION] Impossible OS v0.1.0.42 (a1b2c3d4)
```

## Output

| Artifact | Path | Description |
|----------|------|-------------|
| Kernel ELF | `build/kernel.elf` | Linked kernel binary |
| Initrd | `build/initrd.img` | Initial RAM filesystem |
| ISO | `build/os-build.iso` | Bootable UEFI ISO (GRUB + Multiboot2) |
| Disk image | `build/disk.img` | 64 MiB FAT32 test disk (auto-created) |
