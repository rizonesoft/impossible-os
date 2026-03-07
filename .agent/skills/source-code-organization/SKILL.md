---
description: How to organize source and header files in the Impossible OS project
---

# Source & Header File Organization

## Directory Structure

Impossible OS follows **industry-standard kernel project conventions** (similar to Linux, FreeBSD, Xv6).
Headers mirror the source tree hierarchy under `include/`.

### Include Directory Layout

```
include/
├── kernel/                    # Core kernel headers
│   ├── types.h                # Fundamental integer types
│   ├── printk.h               # Kernel printf
│   ├── log.h                  # Structured logging subsystem
│   ├── gdt.h                  # Global Descriptor Table
│   ├── idt.h                  # Interrupt Descriptor Table
│   ├── elf.h                  # ELF binary format
│   ├── boot_info.h            # Boot information parsing
│   ├── multiboot2.h           # Multiboot2 protocol structs
│   │
│   ├── drivers/               # Hardware driver headers
│   │   ├── serial.h           # COM1 serial port
│   │   ├── keyboard.h         # PS/2 keyboard
│   │   ├── mouse.h            # PS/2 mouse
│   │   ├── framebuffer.h      # GOP framebuffer graphics
│   │   ├── pic.h              # Programmable Interrupt Controller
│   │   ├── pit.h              # Programmable Interval Timer
│   │   ├── ata.h              # ATA/IDE disk driver
│   │   ├── pci.h              # PCI bus enumeration
│   │   ├── rtl8139.h          # RTL8139 NIC driver
│   │   ├── virtio.h           # VirtIO device support
│   │   └── virtio_input.h     # VirtIO input devices
│   │
│   ├── mm/                    # Memory management
│   │   ├── pmm.h              # Physical memory manager
│   │   ├── vmm.h              # Virtual memory manager
│   │   └── heap.h             # Kernel heap (kmalloc/kfree)
│   │
│   ├── fs/                    # Filesystems
│   │   ├── vfs.h              # Virtual filesystem layer
│   │   ├── initrd.h           # Initial RAM filesystem
│   │   ├── fat32.h            # FAT32 driver (EFI partition)
│   │   └── ixfs.h             # IXFS custom filesystem
│   │
│   ├── sched/                 # Scheduler and processes
│   │   ├── task.h             # Task/process management
│   │   └── syscall.h          # System call interface
│   │
│   └── net/                   # Networking
│       └── net.h              # Network stack (Ethernet/ARP/IP/UDP/ICMP/DHCP)
│
└── desktop/                   # Desktop environment headers
    ├── wm.h                   # Window manager
    ├── desktop.h              # Desktop shell (taskbar, start menu)
    ├── font.h                 # Bitmap font renderer
    └── controls.h             # Common controls (Button, Label, TextBox, ScrollBar)
```

### Source Directory Layout

```
src/
├── boot/                      # Bootloader (Multiboot2, Long Mode entry)
│   ├── entry.asm
│   ├── multiboot2_header.asm
│   ├── grub.cfg
│   └── linker.ld
│
├── kernel/                    # Kernel core
│   ├── main.c                 # kernel_main() entry point
│   ├── gdt.c, gdt_asm.asm    # GDT + TSS
│   ├── idt.c, isr_stubs.asm  # IDT + ISR stubs
│   ├── printk.c              # Kernel printf
│   ├── log.c                 # Structured logging
│   ├── elf.c                 # ELF loader
│   │
│   ├── drivers/               # Hardware drivers
│   ├── mm/                    # Memory management
│   ├── fs/                    # Filesystem implementations
│   ├── sched/                 # Scheduler + syscalls
│   └── net/                   # Network stack
│
├── desktop/                   # Desktop environment
│   ├── wm.c                  # Window manager
│   ├── desktop.c             # Desktop shell
│   ├── font.c                # Font renderer
│   └── controls.c            # Common controls
│
└── libc/                      # Minimal kernel libc
```

## Rules for Adding New Files

### Adding a new kernel driver
1. Create `src/kernel/drivers/<name>.c`
2. Create `include/kernel/drivers/<name>.h`
3. Include as: `#include "kernel/drivers/<name>.h"`

### Adding a new filesystem
1. Create `src/kernel/fs/<name>.c`
2. Create `include/kernel/fs/<name>.h`
3. Include as: `#include "kernel/fs/<name>.h"`

### Adding a new desktop component
1. Create `src/desktop/<name>.c`
2. Create `include/desktop/<name>.h`
3. Include as: `#include "desktop/<name>.h"`

### Adding a new kernel subsystem
1. Create `src/kernel/<subsystem>/<name>.c`
2. Create `include/kernel/<subsystem>/<name>.h`
3. Include as: `#include "kernel/<subsystem>/<name>.h"`

## Include Style

- **Always use subdirectory paths**: `#include "kernel/drivers/serial.h"` (NOT `#include "serial.h"`)
- **Use `#pragma once`** for include guards (no `#ifndef` boilerplate)
- **Makefile uses `-Iinclude`** as the include root — all paths are relative to `include/`
- **No circular includes** — use forward declarations when needed

## Naming Conventions

- **Header files**: `snake_case.h` — one header per source file
- **Source files**: `snake_case.c` — match the header name
- **Assembly files**: `snake_case.asm` — NASM syntax, `.asm` extension
