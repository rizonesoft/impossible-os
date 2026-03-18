<p align="center">
  <img src="resources/logo.svg" alt="Impossible OS Logo" width="128" height="128" />
  <br />
  <strong>Impossible OS</strong>
  <br />
  <em>A 64-bit operating system built from scratch for modern x86-64 hardware</em>
</p>

<!-- Dynamic badges (uncomment when repo is public):
  <a href="https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/rizonesoft/impossible-os/build.yml?branch=main&style=flat-square&logo=github&label=Build" alt="Build" /></a>
  <a href="https://github.com/rizonesoft/impossible-os/releases/latest"><img src="https://img.shields.io/github/v/release/rizonesoft/impossible-os?style=flat-square&label=Release&color=green" alt="Release" /></a>
  <a href="https://github.com/rizonesoft/impossible-os/commits/main"><img src="https://img.shields.io/github/last-commit/rizonesoft/impossible-os?style=flat-square&label=Last%20Commit" alt="Last Commit" /></a>
  <a href="https://github.com/rizonesoft/impossible-os/stargazers"><img src="https://img.shields.io/github/stars/rizonesoft/impossible-os?style=flat-square&color=yellow" alt="Stars" /></a>
-->
<p align="center">
  <a href="https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml"><img src="https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml/badge.svg?branch=main" alt="Build" /></a>
  <a href="https://github.com/rizonesoft/impossible-os/releases/latest"><img src="https://img.shields.io/badge/⬇_Download-Latest_Release-2ea44f?style=flat-square" alt="Download" /></a>
  <img src="https://img.shields.io/badge/platform-x86__64-blue?style=flat-square" alt="Platform" />
  <img src="https://img.shields.io/badge/boot-UEFI-00979D?style=flat-square" alt="Boot" />
  <img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square" alt="License" />
  <img src="https://img.shields.io/badge/lines-84k+-blueviolet?style=flat-square" alt="Lines of Code" />
  <a href="https://www.paypal.com/donate/?hosted_button_id=7UGGCSDUZJPFE"><img src="https://img.shields.io/badge/Donate-PayPal-00457C?style=flat-square&logo=paypal" alt="Donate" /></a>
  <a href="https://github.com/sponsors/rizonesoft"><img src="https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?style=flat-square&logo=githubsponsors" alt="Sponsor" /></a>
</p>

---

*"It always seems impossible until it's done."* — Nelson Mandela

They said building a fully functional, feature-rich operating system from scratch was impossible — so we named it after the challenge. Impossible OS is written entirely from the ground up: a custom UEFI bootloader, a 64-bit kernel, a graphical desktop, and everything in between. No Linux kernel, no borrowed foundations, no legacy code. Just bare metal x86-64 and the stubborn belief that impossible is just a word.

<!-- TODO: Add screenshot here once desktop polish is complete -->
<!-- ![Impossible OS Desktop](resources/screenshots/desktop.png) -->

---

## ✨ Feature Highlights

| Feature | Status | Description |
|---------|--------|-------------|
| Custom UEFI bootloader | ✅ | Hand-written PE/COFF application — no GRUB, no shims |
| Secure Boot | ✅ | Shim chain-loading with embedded vendor certificate (`MOK.cer`) |
| 64-bit Long Mode kernel | ✅ | Identity-mapped page tables, GDT/IDT, APIC timers |
| Preemptive multitasking | ✅ | Round-robin scheduler, kernel threads, mutexes, semaphores, seqlocks |
| Virtual filesystem | ✅ | VFS with IXFS (custom), FAT32, GPT, and MBR support |
| Compositing desktop | ✅ | Stacking window manager, dirty-rectangle compositor, double-buffered VSync |
| TrueType fonts | ✅ | Anti-aliased glyph caching via stb_truetype |
| AHCI + VirtIO storage | ✅ | DMA-based disk I/O for real hardware and virtual machines |
| Networking | ✅ | RTL8139 NIC, Ethernet, ARP, IPv4, ICMP ping, UDP, DHCP |
| IPC | ✅ | Pipes, POSIX signals, shared memory |
| Virtual memory | ✅ | Swap/pagefile, mmap, clock page replacement |
| Win32-compatible Registry | ✅ | Hierarchical key-value store with disk persistence |
| User-mode programs | ✅ | ELF loading, syscalls, fork/exec, custom libc |
| BSOD panic screen | ✅ | Register dump, stack trace with symbol resolution, auto-restart |
| Icon system | ✅ | IRES packed format with 9 sizes (16px–256px) |
| Animated cursors | ✅ | Adwaita XCursor theme with 11 cursor types |
| Real hardware boot | ✅ | Tested on Acer Aspire via USB |
| Win32-compatible API | ⬜ | CreateFile, ReadFile, CreateProcess (planned) |
| NTFS support | ⬜ | Read/write NTFS 3.1 driver (planned) |
| TCP/IP stack | ⬜ | Full TCP, DNS, HTTP (planned) |

---

## 🚀 Quick Start

```bash
git clone https://github.com/rizonesoft/impossible-os.git
cd impossible-os
bash scripts/setup.sh          # Install all dependencies
bash scripts/build.sh run      # Build + boot in QEMU
```

> [!NOTE]
> **Requires:** Ubuntu/Debian, Fedora, or Arch Linux (WSL 2 recommended on Windows).
> `setup.sh` installs everything automatically: Clang-19, NASM, QEMU, OVMF, mtools, etc.

### Build Commands

| Command | Description |
|---------|-------------|
| `bash scripts/build.sh` | Incremental build (fast — only changed files) |
| `bash scripts/build.sh clean` | Full clean build |
| `bash scripts/build.sh run` | Incremental build + launch QEMU |
| `bash scripts/build.sh clean run` | Clean build + launch QEMU |

> [!NOTE]
> Always use `bash scripts/build.sh` — never raw `make` commands.
> Verify success: `tail -1 build/build.log` → must show `=== BUILD OK ===`

---

## 🏗️ Architecture

```
UEFI Firmware → BOOTX64.EFI → kernel.exe (ELF) → Desktop Shell
                    │                │
              GOP + MemMap      boot_info struct
              RSDP pointer      framebuffer, memory map
```

### Boot Chain

1. **UEFI firmware** initializes hardware, provides GOP framebuffer
2. **BOOTX64.EFI** (our bootloader) loads kernel ELF, passes `boot_info`
3. **Kernel** sets up GDT, IDT, APIC, PMM, VMM, VFS, drivers
4. **Desktop shell** launches with compositor, taskbar, and terminal

### Memory Model

```
PMM (physical pages) → VMM (page tables) → kmalloc (2 MiB heap)
                                           ↓
                           Small structs only (≤ 4 KB)
                           Everything else → pmm_alloc_contiguous()
```

### Storage Stack

```
AHCI / VirtIO → blkdev → GPT/MBR → FAT32 / IXFS → VFS (C:\, D:\)
```

---

## 📁 Project Structure

```
src/
├── boot/uefi/     Custom UEFI bootloader (PE/COFF, GOP, ELF loader)
├── kernel/        Kernel core
│   ├── drivers/       PCI, AHCI, VirtIO, framebuffer, keyboard, mouse, NIC
│   ├── fs/            VFS, IXFS, FAT32, GPT/MBR partitioning
│   ├── gfx/           2D graphics: blending, gradients, blur, text rendering
│   ├── ipc/           Pipes, signals, shared memory
│   ├── mm/            PMM, VMM, heap, swap, mmap
│   ├── net/           Ethernet, ARP, IPv4, ICMP, UDP, DHCP
│   ├── sched/         Scheduler, threads, syscalls, sync primitives
│   └── smp/           Symmetric multi-processing
├── desktop/       Window manager, compositor, terminal, desktop shell
└── libc/          Minimal kernel libc (placeholder)

include/           All header files (mirrors src/ hierarchy)
resources/         Fonts, icons, wallpapers, cursors
scripts/           Build, test, deploy scripts (13 scripts)
todo/              Development roadmap (100+ TODO items across 50+ files)
```

---

## 🧪 Testing

| Platform | Method |
|----------|--------|
| **QEMU** | `bash scripts/build.sh run` — default, fastest iteration |
| **VirtualBox** | `scripts/emulators/run-vbox.bat` (Windows) or configure a 64-bit EFI VM |
| **Hyper-V** | Gen 2 VM, Secure Boot disabled, UEFI boot from `.vhdx` |
| **Real hardware** | Write `build/system-disk.img` to USB with `dd` or Rufus |

Serial output is written to `build/serial.log` for debugging.

---

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
If your definition of "daily use" involves sudden kernel panics and debugging C at 3 AM, then yes, it's perfect. Otherwise, proceed with extreme caution.

**When will it be finished?**
Sometime between next month and the end of time. Good software takes time; impossible software takes just a little bit longer. 

**Are we stupid or something?**
No. We just have a severe, incurable allergy to people telling us something is "impossible."

---

## 🗺️ Roadmap

Development is tracked in [`todo/TODO-000-INDEX.md`](todo/TODO-000-INDEX.md) — a comprehensive 10-layer roadmap covering everything from kernel foundations to a full application suite.

| Layer | Status |
|-------|--------|
| Infrastructure (build, tooling, CI) | 🔄 In progress |
| Kernel Foundations (boot, memory, scheduler, VFS) | 🔄 In progress |
| Hardware & Drivers (AHCI, USB, NIC) | ⬜ Planned |
| GFX & UI Framework | ⬜ Planned |
| Desktop Shell | ⬜ Planned |
| Core Services | ⬜ Planned |
| Core Apps | ⬜ Planned |
| Multimedia | ⬜ Planned |
| Networking & Internet Apps | ⬜ Planned |
| Polish & Future (DPI, installer, SDK) | ⬜ Planned |

---

## 🙏 Acknowledgments

Impossible OS wouldn't be possible (ironic, we know) without these amazing projects:

- **[stb_truetype](https://github.com/nothings/stb)** — TrueType font rasterization (public domain)
- **[stb_image](https://github.com/nothings/stb)** — JPEG/PNG/BMP decoding (public domain)
- **[OVMF/EDK2](https://github.com/tianocore/edk2)** — UEFI firmware for testing (BSD-2-Clause)
- **[SerenityOS](https://github.com/SerenityOS/serenity)** — Inspiration for what a solo OS project can become
- **[OSDev Wiki](https://wiki.osdev.org/)** — The encyclopedia of OS development
- **[Adwaita](https://gitlab.gnome.org/GNOME/adwaita-icon-theme)** — Cursor theme (LGPL/CC-BY-SA)
- **[Inter](https://rsms.me/inter/)** & **[FluentSystemIcons](https://github.com/microsoft/fluentui-system-icons)** — Fonts and icons

---

## 📄 License

GPL-3.0 License — see [LICENSE](LICENSE) for details.

Copyright © 2026 [Rizonesoft](https://github.com/rizonesoft)
