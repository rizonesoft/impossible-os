# Impossible OS

*"You can't write a complete PC operating system from scratch — it's impossible."*

Everyone says building a fully functional, feature-rich operating system is impossible. Impossible OS exists to prove them wrong. Written entirely from scratch — from UEFI bootloader to graphical desktop — with no legacy code, no Linux kernel, no borrowed foundations. Just bare metal x86-64 and a refusal to accept "impossible."

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

## 🤔 Frequently Asked Questions (FAQ)

**Is it actually possible to write a custom OS entirely from scratch?**
Technically, no. Practically, also no. But we're doing it anyway. 

**Can a solo developer really compete against a 500+ person corporate engineering team?**
Absolutely not. But why let a little thing like impossible odds get in the way? We're giving it a go anyway. 

**Will this work seamlessly like Windows 11?**
Highly unlikely. We might not even have a functioning classic control panel right away, but that isn't going to stop us from trying to build a better experience from the ground up.

**Do we really need yet another operating system in the world?**
No. The market is completely saturated. But here it is anyway. Enjoy.

**Is Impossible OS stable enough for daily use?**
If your definition of "daily use" involves sudden kernel panics and debugging C++ at 3 AM, then yes, it's perfect. Otherwise, proceed with extreme caution.

**When will it be finished?**
Sometime between next month and the end of time. Good software takes time; impossible software takes just a little bit longer. 

**Are we stupid or something?**
No. We just have a severe, incurable allergy to people telling us something is "impossible."

## License

MIT License — see [LICENSE](LICENSE) for details.
