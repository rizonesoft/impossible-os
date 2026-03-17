# Impossible OS

*"It always seems impossible until it's done."* — Nelson Mandela

They said building a fully functional, feature-rich operating system from scratch was impossible — so we named it after the challenge. Impossible OS is written entirely from the ground up: a custom UEFI bootloader, a 64-bit kernel, a graphical desktop, and everything in between. No Linux kernel, no borrowed foundations, no legacy code. Just bare metal x86-64 and the stubborn belief that impossible is just a word.

## Features

- **Custom UEFI bootloader** — hand-written PE/COFF boot application (no GRUB)
- **Secure Boot** — shim chain-loading with embedded vendor certificate (`MOK.cer`); pending Microsoft shim-review signing
- **64-bit Long Mode** kernel with identity-mapped page tables
- **Preemptive multitasking** with round-robin scheduler, kernel threads, mutexes, and semaphores
- **Virtual filesystem** with IXFS (custom filesystem), FAT32, GPT, and MBR support
- **Graphical desktop** with stacking window manager, dirty-rectangle compositor, and double-buffered VSync
- **TrueType font rendering** via stb_truetype with anti-aliased glyph caching
- **Windows 11-style boot splash** with animated dots, embedded TTF font, and status messages
- **AHCI & VirtIO** block device drivers (real hardware + virtual machines)
- **PS/2 keyboard & mouse** drivers with VirtIO tablet for absolute positioning
- **Networking** — RTL8139 NIC driver, Ethernet, ARP, IPv4, ICMP (ping), UDP, DHCP client
- **IPC** — pipes, signals (SIGINT/SIGTERM/SIGKILL), shared memory
- **Virtual memory** — swap/pagefile, memory-mapped files, clock page replacement
- **Kernel panic screen** — styled BSOD with register dump, stack trace, and auto-restart
- **Codex registry** — hierarchical key-value store with disk persistence (migrating to Win32-compatible Registry)
- **User-mode programs** — ELF loading, syscalls, fork/exec, custom libc
- **Multi-resolution icon system** — IRES packed icon format with 9 sizes (16px–256px)
- **Adwaita cursors** — animated cursor support with 11 cursor types

## Getting Started

```bash
git clone https://github.com/rizonesoft/impossible-os.git
cd impossible-os
bash scripts/setup.sh          # Install deps + verify build
bash scripts/build.sh run      # Boot in QEMU
```

> [!NOTE]
> **Requires:** Ubuntu/Debian, Fedora, or Arch Linux (WSL 2 recommended on Windows).
> `setup.sh` installs everything automatically: Clang-19, NASM, QEMU, OVMF, mtools, etc.

### Build commands

```bash
bash scripts/build.sh          # Incremental build
bash scripts/build.sh clean    # Clean build
bash scripts/build.sh run      # Build + QEMU
bash scripts/build.sh clean run  # Clean build + QEMU
```

> [!NOTE]
> Always use `bash scripts/build.sh` — never raw `make` commands.
> Verify success: `tail -1 build/build.log` → must show `BUILD OK`.

## Testing

- **Fast loop:** `bash scripts/build.sh run` launches QEMU with OVMF UEFI firmware and AHCI
- **Serial output:** Boot log written to `build/serial.log` (`-serial file:build/serial.log`)
- **VirtualBox:** Use `scripts/emulators/run-vbox.bat` (Windows) or configure a 64-bit EFI VM manually

## Architecture

```
src/
├── boot/uefi/  # Custom UEFI bootloader (PE/COFF, GOP, ELF loader)
├── kernel/     # Core kernel
│   ├── drivers/    # PCI, AHCI, VirtIO, framebuffer, keyboard, mouse, PIT, RTC
│   ├── fs/         # VFS, IXFS, FAT32, GPT/MBR partitioning
│   ├── gfx/        # Graphics library (blending, gradients, text rendering)
│   ├── ipc/        # Pipes, signals, shared memory
│   ├── mm/         # PMM, heap, virtual memory, swap, mmap
│   ├── net/        # Ethernet, ARP, IPv4, ICMP, UDP, DHCP
│   └── sched/      # Scheduler, threads, syscalls, user-mode
├── desktop/    # Window manager, compositor, terminal, desktop shell
└── installer/  # OS installer (planned)
user/
├── lib/        # User-mode libc (string, stdio, stdlib, ctype, math)
├── hello.c     # Hello world test program
└── shell.c     # User-mode shell
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
