# Impossible OS

A freestanding x86-64 operating system built from scratch — UEFI bootloader through graphical desktop.

## Features (Planned)

- **UEFI-only boot** via GRUB + Multiboot2
- **64-bit Long Mode** kernel
- **Preemptive multitasking** with round-robin scheduler
- **Virtual filesystem** with FAT32 support
- **Graphical desktop** with stacking window manager
- **PS/2 keyboard & mouse** drivers
- **Networking** (UDP/TCP/IP stack)
- **OS installer** with GPT partitioning

## Build Requirements

- Ubuntu 24.04 (WSL 2 recommended)
- GCC cross-compiler (`x86_64-elf-gcc`) or system GCC with freestanding flags
- NASM assembler
- GRUB + xorriso + mtools (for ISO creation)
- QEMU + OVMF (for UEFI testing)

## Quick Start

```bash
# Build everything
make all

# Create bootable ISO
make iso

# Test in QEMU (UEFI)
make run

# Clean build artifacts
make clean
```

## Testing

- **Fast loop:** `make run` launches QEMU with OVMF UEFI firmware
- **Production:** Attach `build/os-build.iso` to a Hyper-V Gen 2 VM

## Architecture

```
src/
├── boot/       # GRUB config, Multiboot2 header, Long Mode entry
├── kernel/     # Core kernel (interrupts, GDT, drivers, mm, fs, sched)
├── libc/       # Minimal C standard library
├── shell/      # Command-line shell
├── desktop/    # Window manager, GUI toolkit, desktop shell
└── installer/  # OS installer
```

## License

MIT License — see [LICENSE](LICENSE) for details.
