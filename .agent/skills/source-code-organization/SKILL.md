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
├── build_info.h               # Auto-generated build metadata
├── gfx.h                     # 2D graphics library (blending, shapes, effects)
├── gfx_simd.h                # SSE2 SIMD-accelerated graphics routines
├── font_mgr.h                # TrueType font manager (slot-based caching)
├── icon_store.h               # IRES icon pack loader + colored icon renderer
├── ico.h                      # Windows ICO file parser
├── registry.h                 # Win32-compatible Registry API
├── stb_image.h                # stb_image (JPEG/PNG/BMP/GIF decode)
├── stb_truetype.h             # stb_truetype (TTF parsing/rasterization)
├── cursor.h                   # Hardware cursor management
│
├── kernel/                    # Core kernel headers
│   ├── types.h                # Fundamental integer types
│   ├── printk.h               # Kernel printf
│   ├── klog.h                 # Structured klog() logging
│   ├── panic.h                # Kernel panic + BSOD + crash dump
│   ├── acpi.h                 # ACPI table parsing (RSDP, MADT, FADT)
│   ├── boot_info.h            # UEFI boot_info struct (GOP, memory map, RSDP)
│   ├── gdt.h                  # Global Descriptor Table
│   ├── idt.h                  # Interrupt Descriptor Table
│   ├── elf.h                  # ELF binary format
│   ├── image.h                # Runtime image decode + scaling
│   ├── rcu.h                  # Read-Copy-Update synchronization
│   ├── atomic.h               # Atomic operations
│   │
│   ├── drivers/               # Hardware driver headers
│   │   ├── serial.h           # COM1 serial port
│   │   ├── keyboard.h         # PS/2 keyboard
│   │   ├── mouse.h            # PS/2 mouse
│   │   ├── framebuffer.h      # UEFI GOP framebuffer
│   │   ├── lapic.h            # Local APIC timer + IPI
│   │   ├── ioapic.h           # I/O APIC interrupt routing
│   │   ├── pic.h              # Legacy PIC (boot-time disable only)
│   │   ├── pit.h              # Programmable Interval Timer
│   │   ├── rtc.h              # Real-Time Clock
│   │   ├── ahci.h             # AHCI (SATA DMA) disk driver
│   │   ├── blkdev.h           # Block device abstraction layer
│   │   ├── pci.h              # PCI bus enumeration
│   │   ├── virtio.h           # VirtIO device support
│   │   ├── virtio_blk.h       # VirtIO block device
│   │   └── virtio_input.h     # VirtIO input devices
│   │
│   ├── mm/                    # Memory management
│   │   ├── pmm.h              # Physical memory manager
│   │   ├── vmm.h              # Virtual memory manager
│   │   ├── heap.h             # Kernel heap (kmalloc/kfree)
│   │   ├── swap.h             # Swap / page file
│   │   └── mmap.h             # Memory-mapped files
│   │
│   ├── fs/                    # Filesystems
│   │   ├── vfs.h              # Virtual filesystem layer
│   │   ├── ixfs.h             # IXFS in-memory filesystem
│   │   ├── fat32.h            # FAT32 driver
│   │   ├── gpt.h              # GPT partition table parser
│   │   ├── mbr.h              # MBR partition table parser
│   │   └── partition.h        # Partition discovery
│   │
│   ├── sched/                 # Scheduler and processes
│   │   ├── task.h             # Task/process management
│   │   ├── syscall.h          # System call interface
│   │   ├── mutex.h            # Mutex
│   │   ├── rwlock.h           # Read-write lock
│   │   ├── condvar.h          # Condition variable
│   │   └── semaphore.h        # Counting semaphore
│   │
│   ├── ipc/                   # Inter-process communication
│   │   ├── pipe.h             # Anonymous pipes
│   │   ├── signal.h           # POSIX-style signals
│   │   └── shmem.h            # Shared memory
│   │
│   └── net/                   # Networking
│       └── net.h              # Network stack (Ethernet/ARP/IP/UDP/ICMP/DHCP)
│
├── desktop/                   # Desktop environment headers
│   ├── wm.h                   # Window manager
│   ├── desktop.h              # Desktop shell (taskbar, start menu)
│   ├── terminal.h             # Terminal emulator
│   ├── font.h                 # Bitmap font renderer (early boot fallback)
│   ├── controls.h             # Common controls (Button, Label, TextBox)
│   └── gallery.h              # Image gallery viewer

└── generated/                 # Build-generated headers
    └── os_logo_*.h            # Pre-built icon BGRA arrays
```

### Source Directory Layout

```
src/
├── boot/                      # Bootloader
│   ├── uefi/                  # Custom UEFI bootloader (PE32+)
│   │   ├── bootx64.c         # UEFI application — GOP, memory map, kernel load
│   │   ├── efi.h             # UEFI protocol definitions
│   │   ├── reloc.asm          # PE relocation fixups
│   │   └── uefi.lds           # UEFI linker script
│   ├── entry.asm              # Kernel entry point (Long Mode setup)
│   └── linker.ld              # Kernel linker script
│
├── kernel/                    # Kernel core
│   ├── main.c                 # kernel_main() entry point
│   ├── gdt.c, gdt_asm.asm    # GDT + TSS
│   ├── idt.c, isr_stubs.asm  # IDT + ISR stubs
│   ├── printk.c              # Kernel printf
│   ├── klog.c                # Structured logging
│   ├── panic.c               # Kernel panic + BSOD
│   ├── acpi.c                # ACPI table parsing
│   ├── registry.c            # Win32-compatible Registry
│   ├── elf.c                 # ELF loader
│   ├── image.c               # Runtime JPEG/PNG decode
│   ├── icon_store.c           # IRES icon pack
│   │
│   ├── drivers/               # Hardware drivers
│   ├── mm/                    # Memory management
│   ├── fs/                    # Filesystem implementations (IXFS, FAT32, VFS)
│   ├── sched/                 # Scheduler + syscalls + sync primitives
│   ├── ipc/                   # Pipes, signals, shared memory
│   ├── gfx/                   # 2D graphics library (blend, blur, effects)
│   ├── net/                   # Network stack
│   └── test/                  # Kernel unit tests
│
├── desktop/                   # Desktop environment
│   ├── wm.c                  # Window manager + dirty rectangle compositor
│   ├── desktop.c             # Desktop shell (wallpaper, taskbar, start menu)
│   ├── terminal.c            # Terminal emulator
│   ├── font.c                # Bitmap font (early boot fallback)
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
